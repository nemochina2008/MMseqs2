// Computes MSAs from clustering or alignment result

#include <string>
#include <vector>
#include <sstream>
#include <sys/time.h>

#include "MsaFilter.h"
#include "Parameters.h"
#include "PSSMCalculator.h"
#include "DBReader.h"
#include "DBConcat.h"
#include "DBWriter.h"
#include "HeaderSummarizer.h"
#include "CompressedA3M.h"
#include "Debug.h"
#include "Util.h"

#ifdef OPENMP
#include <omp.h>
#endif

int result2msa(Parameters &par, const std::string &outpath,
                      const size_t dbFrom, const size_t dbSize, DBConcat *referenceDBr = NULL) {
#ifdef OPENMP
    omp_set_num_threads(par.threads);
#endif

    if (par.compressMSA && referenceDBr == NULL) {
        Debug(Debug::ERROR) << "Need a sequence and header database for ca3m output!\n";
        EXIT(EXIT_FAILURE);
    }

    DBReader<unsigned int> qDbr(par.db1.c_str(), par.db1Index.c_str());
    qDbr.open(DBReader<unsigned int>::NOSORT);

    std::string headerNameQuery(par.db1);
    headerNameQuery.append("_h");

    std::string headerIndexNameQuery(par.db1);
    headerIndexNameQuery.append("_h.index");

    DBReader<unsigned int> queryHeaderReader(headerNameQuery.c_str(), headerIndexNameQuery.c_str());
    // NOSORT because the index should be in the same order as resultReader
    queryHeaderReader.open(DBReader<unsigned int>::NOSORT);

    DBReader<unsigned int> *tDbr = &qDbr;
    DBReader<unsigned int> *tempateHeaderReader = &queryHeaderReader;

    unsigned int maxSequenceLength = 0;
    const bool sameDatabase = (par.db1.compare(par.db2) == 0) ? true : false;
    if (!sameDatabase) {
        tDbr = new DBReader<unsigned int>(par.db2.c_str(), par.db2Index.c_str());
        tDbr->open(DBReader<unsigned int>::SORT_BY_LINE);

        unsigned int *lengths = tDbr->getSeqLens();
        for (size_t i = 0; i < tDbr->getSize(); i++) {
            maxSequenceLength = std::max(lengths[i], maxSequenceLength);
        }

        std::string headerNameTarget(par.db2);
        headerNameTarget.append("_h");

        std::string headerIndexNameTarget(par.db2);
        headerIndexNameTarget.append("_h.index");

        tempateHeaderReader = new DBReader<unsigned int>(headerNameTarget.c_str(), headerIndexNameTarget.c_str());
        tempateHeaderReader->open(DBReader<unsigned int>::NOSORT);
    } else {
        unsigned int *lengths = qDbr.getSeqLens();
        for (size_t i = 0; i < qDbr.getSize(); i++) {
            maxSequenceLength = std::max(lengths[i], maxSequenceLength);
        }
    }


    bool firstSeqRepr = sameDatabase && par.compressMSA;
    if (firstSeqRepr) {
        Debug(Debug::INFO) << "Using the first target sequence as center sequence for making each alignment.\n";
    }

    DBReader<unsigned int> resultReader(par.db3.c_str(), par.db3Index.c_str());
    resultReader.open(DBReader<unsigned int>::LINEAR_ACCCESS);

    DBWriter resultWriter(outpath.c_str(), std::string(outpath + ".index").c_str(), par.threads, DBWriter::BINARY_MODE);
    resultWriter.open();

    size_t maxSetSize = resultReader.maxCount('\n');

    // adjust score of each match state by -0.2 to trim alignment
    SubstitutionMatrix subMat(par.scoringMatrixFile.c_str(), 2.0f, -0.2f);

    Debug(Debug::INFO) << "Start computing "
                       << (par.compressMSA ? "compressed" : "") << " multiple sequence alignments.\n";
    EvalueComputation evalueComputation(tDbr->getAminoAcidDBSize(), &subMat, Matcher::GAP_OPEN, Matcher::GAP_EXTEND,
                                        true);
#pragma omp parallel
    {
        Matcher matcher(maxSequenceLength, &subMat, &evalueComputation, par.compBiasCorrection);

        MultipleAlignment aligner(maxSequenceLength, maxSetSize, &subMat, &matcher);
        PSSMCalculator calculator(&subMat, maxSequenceLength, maxSetSize, par.pca, par.pcb);
        MsaFilter filter(maxSequenceLength, maxSetSize, &subMat);
        UniprotHeaderSummarizer summarizer;

        int sequenceType = Sequence::AMINO_ACIDS;
        if (par.queryProfile == true) {
            sequenceType = Sequence::HMM_PROFILE;
        }

        Sequence centerSequence(maxSequenceLength, subMat.aa2int, subMat.int2aa,
                                sequenceType, 0, false, par.compBiasCorrection);

#pragma omp for schedule(static)
        for (size_t id = dbFrom; id < (dbFrom + dbSize); id++) {
            Debug::printProgress(id);
            unsigned int thread_idx = 0;
#ifdef OPENMP
            thread_idx = (unsigned int) omp_get_thread_num();
#endif

            // Get the sequence from the queryDB
            unsigned int queryKey = resultReader.getDbKey(id);
            char *seqData = qDbr.getDataByDBKey(queryKey);
            if (seqData == NULL) {
                Debug(Debug::WARNING) << "Empty sequence " << id << ". Skipping.\n";
                continue;
            }

            unsigned int centerSequenceKey = 0;
            char *centerSequenceHeader = NULL;
            if (!firstSeqRepr) {
                centerSequence.mapSequence(0, queryKey, seqData);
                centerSequenceKey = queryKey;
                centerSequenceHeader = queryHeaderReader.getDataByDBKey(centerSequenceKey);
            }

            char *results = resultReader.getData(id);
            std::vector<Matcher::result_t> alnResults;
            std::vector<Sequence *> seqSet;
            char dbKey[255 + 1];

            bool hasCenter = false;
            while (*results != '\0') {
                Util::parseKey(results, dbKey);
                const unsigned int key = (unsigned int) strtoul(dbKey, NULL, 10);
                char *entry[255];
                const size_t columns = Util::getWordsOfLine(results, entry, 255);

                const size_t edgeId = tDbr->getId(key);
                char *dbSeqData = tDbr->getData(edgeId);

                if (!hasCenter) {
                    centerSequence.mapSequence(0, key, dbSeqData);
                    centerSequenceKey = key;
                    centerSequenceHeader = tempateHeaderReader->getDataByDBKey(centerSequenceKey);
                    hasCenter = true;
                }

                // just add sequences if eval < thr. and if key is not the same as the query in case of sameDatabase
                if ((key != queryKey || sameDatabase == false)) {
                    if (columns > Matcher::ALN_RES_WITH_OUT_BT_COL_CNT) {
                        Matcher::result_t res = Matcher::parseAlignmentRecord(results);
                        if (!(res.dbKey == centerSequence.getDbKey() && sameDatabase == true)) {
                            alnResults.push_back(res);
                        }
                    }

                    Sequence *edgeSequence = new Sequence(tDbr->getSeqLens(edgeId), subMat.aa2int, subMat.int2aa,
                                                          Sequence::AMINO_ACIDS, 0, false, false);
                    edgeSequence->mapSequence(0, key, dbSeqData);
                    seqSet.push_back(edgeSequence);
                }
                results = Util::skipLine(results);
            }

            // Recompute the backtrace if the center seq has to be the first seq
            // or if not all the backtraces are present
            MultipleAlignment::MSAResult res = (firstSeqRepr == false && alnResults.size() == seqSet.size())
                                               ? aligner.computeMSA(&centerSequence, seqSet, alnResults, !par.allowDeletion)
                                               : aligner.computeMSA(&centerSequence, seqSet, !par.allowDeletion);
            //MultipleAlignment::print(res, &subMat);

            alnResults = res.alignmentResults;
            size_t originalSetSize = res.setSize;
            if (par.filterMsa == 1) {
                MsaFilter::MsaFilterResult filterRes = filter.filter((const char **) res.msaSequence, res.setSize,
                                                                     res.centerLength, static_cast<int>(par.cov * 100),
                                                                     static_cast<int>(par.qid * 100), par.qsc,
                                                                     static_cast<int>(par.filterMaxSeqId * 100),
                                                                     par.Ndiff);
                res.keep = (char *) filterRes.keep;
                for (size_t i = 0; i < res.setSize; i++) {
                    if (res.keep[i] == 0) {
                        free(res.msaSequence[i]);
                    }
                }
                for (size_t i = 0; i < filterRes.setSize; i++) {
                    res.msaSequence[i] = (char *) filterRes.filteredMsaSequence[i];
                }
                res.setSize = filterRes.setSize;
            }
            char *data;
            size_t dataSize;
            std::ostringstream msa;
            std::string result;
            if (!par.compressMSA) {
                if (par.summarizeHeader) {
                    // gather headers for summary
                    std::vector<std::string> headers;
                    for (size_t i = 0; i < res.setSize; i++) {
                        if (i == 0) {
                            headers.push_back(centerSequenceHeader);
                        } else {
                            headers.push_back(tempateHeaderReader->getDataByDBKey(seqSet[i]->getDbKey()));
                        }
                    }

                    std::string summary = summarizer.summarize(headers);
                    msa << "#" << par.summaryPrefix.c_str() << "-" << centerSequenceKey << "|" << summary.c_str()
                        << "\n";
                }

                size_t start = 0;
                if (par.skipQuery == true) {
                    start = 1;
                }
                for (size_t i = start; i < res.setSize; i++) {
                    unsigned int key;
                    char *header;
                    if (i == 0) {
                        key = centerSequenceKey;
                        header = centerSequenceHeader;
                    } else {
                        key = seqSet[i - 1]->getDbKey();
                        header = tempateHeaderReader->getDataByDBKey(key);
                    }
                    if (par.addInternalId) {
                        msa << "#" << key << "\n";
                    }

                    msa << ">" << header;

                    // need to allow insertion in the centerSequence
                    for (size_t pos = 0; pos < res.centerLength; pos++) {
                        char aa = res.msaSequence[i][pos];
                        msa << ((aa < MultipleAlignment::NAA) ? subMat.int2aa[(int) aa] : '-');
                    }

                    msa << "\n";
                }
                result = msa.str();

                data = (char *) result.c_str();
                dataSize = result.length();
            } else {
                // Put the query sequence (master sequence) first in the alignment
                Matcher::result_t firstSequence;
                firstSequence.dbKey = centerSequenceKey;
                firstSequence.qStartPos = 0;
                firstSequence.dbStartPos = 0;
                firstSequence.backtrace = std::string(centerSequence.L, 'M'); // only matches

                alnResults.insert(alnResults.begin(), firstSequence);

                if (par.omitConsensus == false) {
                    std::pair<const char *, std::string> pssmRes =
                            calculator.computePSSMFromMSA(res.setSize, res.centerLength,
                                                          (const char **) res.msaSequence, par.wg);
                    msa << ">consensus_" << queryHeaderReader.getDataByDBKey(queryKey) << pssmRes.second << "\n;";
                } else {
                    std::ostringstream centerSeqStr;
                    // Retrieve the master sequence
                    for (int pos = 0; pos < centerSequence.L; pos++) {
                        centerSeqStr << subMat.int2aa[centerSequence.int_sequence[pos]];
                    }
                    msa << ">" << queryHeaderReader.getDataByDBKey(queryKey) << centerSeqStr.str() << "\n;";
                }

                msa << CompressedA3M::fromAlignmentResult(alnResults, *referenceDBr);

                result = msa.str();
                data = (char *) result.c_str();
                dataSize = result.length();
            }

            resultWriter.writeData(data, dataSize, queryKey, thread_idx);

            res.setSize = originalSetSize;
            MultipleAlignment::deleteMSA(&res);
            for (std::vector<Sequence *>::iterator it = seqSet.begin(); it != seqSet.end(); ++it) {
                Sequence *seq = *it;
                delete seq;
            }
        }
    }

    // cleanup
    resultWriter.close();
    resultReader.close();
    queryHeaderReader.close();
    qDbr.close();

    if (!sameDatabase) {
        tempateHeaderReader->close();
        delete tempateHeaderReader;
        tDbr->close();
        delete tDbr;
    }

    Debug(Debug::INFO) << "\nDone.\n";

    return EXIT_SUCCESS;
}

int result2msa(Parameters &par) {
    DBReader<unsigned int> *resultReader = new DBReader<unsigned int>(par.db3.c_str(), par.db3Index.c_str());
    resultReader->open(DBReader<unsigned int>::NOSORT);
    size_t resultSize = resultReader->getSize();
    resultReader->close();
    delete resultReader;

    std::string outname = par.db4;
    DBConcat *referenceDBr = NULL;
    if (par.compressMSA) {
        std::string referenceSeqName(outname);
        std::string referenceSeqIndexName(outname);
        referenceSeqName.append("_sequence.ffdata");
        referenceSeqIndexName.append("_sequence.ffindex");

        // Use only 1 thread for concat to ensure the same order as the later header concat
        referenceDBr = new DBConcat(par.db1, par.db1Index, par.db2, par.db2Index,
                                    referenceSeqName, referenceSeqIndexName, 1);
        referenceDBr->concat();
        // When exporting in ca3m,
        // we need to have an access in SORT_BY_LINE
        // mode in order to keep track of the original
        // line number in the index file.
        referenceDBr->open(DBReader<unsigned int>::SORT_BY_LINE);

        std::string headerQuery(par.db1);
        headerQuery.append("_h");
        std::pair<std::string, std::string> query = Util::databaseNames(headerQuery);

        std::string headerTarget(par.db2);
        headerTarget.append("_h");
        std::pair<std::string, std::string> target = Util::databaseNames(headerTarget);

        std::string referenceHeadersName(outname);
        std::string referenceHeadersIndexName(outname);

        referenceHeadersName.append("_header.ffdata");
        referenceHeadersIndexName.append("_header.ffindex");

        // Use only 1 thread for concat to ensure the same order as the former sequence concat
        DBConcat referenceHeadersDBr(query.first, query.second, target.first, target.second,
                                     referenceHeadersName, referenceHeadersIndexName, 1);
        referenceHeadersDBr.concat();

        outname.append("_ca3m");
    }

    int status = result2msa(par, outname, 0, resultSize, referenceDBr);


    if (referenceDBr != NULL) {
        referenceDBr->close();
        delete referenceDBr;
    }
    return status;
}

int result2msa(Parameters &par, const unsigned int mpiRank, const unsigned int mpiNumProc) {
    DBReader<unsigned int> *qDbr = new DBReader<unsigned int>(par.db3.c_str(), par.db3Index.c_str());
    qDbr->open(DBReader<unsigned int>::NOSORT);

    size_t dbFrom = 0;
    size_t dbSize = 0;
    Util::decomposeDomainByAminoAcid(qDbr->getAminoAcidDBSize(), qDbr->getSeqLens(), qDbr->getSize(),
                                     mpiRank, mpiNumProc, &dbFrom, &dbSize);
    qDbr->close();
    delete qDbr;


    Debug(Debug::INFO) << "Compute split from " << dbFrom << " to " << dbFrom + dbSize << "\n";

    DBConcat *referenceDBr = NULL;
    std::string outname = par.db4;
    if (par.compressMSA) {
        std::string referenceSeqName(outname);
        std::string referenceSeqIndexName(outname);
        referenceSeqName.append("_sequence.ffdata");
        referenceSeqIndexName.append("_sequence.ffindex");

        // Use only 1 thread for concat to ensure the same order as the later header concat
        referenceDBr = new DBConcat(par.db1, par.db1Index, par.db2, par.db2Index,
                                    referenceSeqName, referenceSeqIndexName, 1);
        if (mpiRank == 0) {
            referenceDBr->concat();
        } else {
            referenceDBr->concat(false);
        }

#ifdef HAVE_MPI
        MPI_Barrier(MPI_COMM_WORLD);
#endif
        // When exporting in ca3m,
        // we need to have an access in SORT_BY_LINE
        // mode in order to keep track of the original
        // line number in the index file.
        referenceDBr->open(DBReader<unsigned int>::SORT_BY_LINE);

        if (mpiRank == 0) {
            std::string headerQuery(par.db1);
            headerQuery.append("_h");
            std::pair<std::string, std::string> query = Util::databaseNames(headerQuery);

            std::string headerTarget(par.db2);
            headerTarget.append("_h");
            std::pair<std::string, std::string> target = Util::databaseNames(headerTarget);

            std::string referenceHeadersName(outname);
            std::string referenceHeadersIndexName(outname);

            referenceHeadersName.append("_header.ffdata");
            referenceHeadersIndexName.append("_header.ffindex");

            // Use only 1 thread for concat to ensure the same order as the former sequence concat
            DBConcat referenceHeadersDBr(query.first, query.second, target.first, target.second,
                                         referenceHeadersName, referenceHeadersIndexName, 1);
            referenceHeadersDBr.concat();
        }

        outname.append("_ca3m");
    }

    std::pair<std::string, std::string> tmpOutput = Util::createTmpFileNames(outname, "", mpiRank);
    int status = result2msa(par, tmpOutput.first, dbFrom, dbSize, referenceDBr);

    // close reader to reduce memory
    if (referenceDBr != NULL) {
        referenceDBr->close();
        delete referenceDBr;
    }

#ifdef HAVE_MPI
    MPI_Barrier(MPI_COMM_WORLD);
#endif

    // master reduces results
    if (mpiRank == 0) {
        std::vector<std::pair<std::string, std::string> > splitFiles;
        for (unsigned int procs = 0; procs < mpiNumProc; procs++) {
            std::pair<std::string, std::string> tmpFile = Util::createTmpFileNames(outname, "", procs);
            splitFiles.push_back(std::make_pair(tmpFile.first, tmpFile.first + ".index"));

        }
        // merge output ffindex databases
        DBWriter::mergeResults(outname, outname + ".index", splitFiles);
    }

    return status;
}

int result2msa(int argc, const char **argv, const Command &command) {
    MMseqsMPI::init(argc, argv);

    Parameters &par = Parameters::getInstance();
    // do not filter as default
    par.filterMsa = 0;
    par.parseParameters(argc, argv, command, 4);

    struct timeval start, end;
    gettimeofday(&start, NULL);

#ifdef HAVE_MPI
    int status = result2msa(par, MMseqsMPI::rank, MMseqsMPI::numProc);
#else
    int status = result2msa(par);
#endif

    gettimeofday(&end, NULL);
    time_t sec = end.tv_sec - start.tv_sec;
    Debug(Debug::WARNING) << "Time for processing: " << (sec / 3600) << " h " << (sec % 3600 / 60) << " m "
                          << (sec % 60) << "s\n";

    return status;
}
