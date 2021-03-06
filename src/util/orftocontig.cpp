#include "Parameters.h"
#include "Matcher.h"
#include "DBReader.h"
#include "DBWriter.h"
#include "Orf.h"
#include "Debug.h"
#include "Util.h"

#ifdef OPENMP
#include <omp.h>
#endif

int orftocontig(int argn, const char **argv, const Command& command) {
    Parameters& par = Parameters::getInstance();
    par.parseParameters(argn, argv, command, 3, true, true);

    // contig length is needed for computation:
    DBReader<unsigned int> contigsReader(par.db1.c_str(), par.db1Index.c_str());
    contigsReader.open(DBReader<unsigned int>::NOSORT);

    // info will be obtained from orf headers:
    DBReader<unsigned int> orfHeadersReader(par.hdr2.c_str(), par.hdr2Index.c_str());
    orfHeadersReader.open(DBReader<unsigned int>::LINEAR_ACCCESS);

    // writing in alignment format:
    DBWriter alignmentFormatWriter(par.db3.c_str(), par.db3Index.c_str(), par.threads);
    alignmentFormatWriter.open();

#pragma omp parallel
    {
        unsigned int thread_idx = 0;
#ifdef OPENMP
        thread_idx = static_cast<unsigned int>(omp_get_thread_num());
#endif 
        char orfToContigBuffer[LINE_MAX];
        
#pragma omp for schedule(dynamic, 100)
        for (size_t id = 0; id < orfHeadersReader.getSize(); ++id) {
            Debug::printProgress(id);
            unsigned int orfKey = orfHeadersReader.getDbKey(id);
            Matcher::result_t orfToContigResult = Orf::getFromDatabase(id, contigsReader, orfHeadersReader);           
            size_t len = Matcher::resultToBuffer(orfToContigBuffer, orfToContigResult, true);
            alignmentFormatWriter.writeData(orfToContigBuffer, len, orfKey, thread_idx);
        }
    }
    
    // cleanup
    alignmentFormatWriter.close();
    orfHeadersReader.close();
    contigsReader.close();

    return EXIT_SUCCESS;
}
