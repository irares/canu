
/******************************************************************************
 *
 *  This file is part of canu, a software program that assembles whole-genome
 *  sequencing reads into contigs.
 *
 *  This software is based on:
 *    'Celera Assembler' (http://wgs-assembler.sourceforge.net)
 *    the 'kmer package' (http://kmer.sourceforge.net)
 *  both originally distributed by Applera Corporation under the GNU General
 *  Public License, version 2.
 *
 *  Canu branched from Celera Assembler at its revision 4587.
 *  Canu branched from the kmer project at its revision 1994.
 *
 *  This file is derived from:
 *
 *    src/stores/gkStore.C
 *
 *  Modifications by:
 *
 *    Brian P. Walenz beginning on 2017-OCT-03
 *      are a 'United States Government Work', and
 *      are released in the public domain
 *
 *    Sergey Koren beginning on 2017-OCT-18
 *      are a 'United States Government Work', and
 *      are released in the public domain
 *
 *  File 'README.licenses' in the root directory of this distribution contains
 *  full conditions and disclaimers for each license.
 */

#include "gkStore.H"

#include "AS_UTL_fileIO.H"



void
gkStore::gkStore_loadMetadata(void) {
  char    name[FILENAME_MAX+1];

  _librariesAlloc = _info.gkInfo_numLibraries() + 1;
  _readsAlloc     = _info.gkInfo_numReads()     + 1;

  _libraries      = new gkLibrary [_librariesAlloc];
  _reads          = new gkRead    [_readsAlloc];

  AS_UTL_loadFile(_storePath, '/', "libraries", _libraries, _librariesAlloc);
  AS_UTL_loadFile(_storePath, '/', "reads",     _reads,     _readsAlloc);
}










gkStore::gkStore(char const    *storePath,
                 char const    *clonePath,
                 gkStore_mode   mode,
                 uint32         partID) {
  char    nameI[FILENAME_MAX+1];
  char    nameL[FILENAME_MAX+1];
  char    nameR[FILENAME_MAX+1];
  char    nameB[FILENAME_MAX+1];

  //  Clear ourself, to make valgrind happier.

  memset(_storePath, 0, sizeof(char) * (FILENAME_MAX + 1));
  memset(_clonePath, 0, sizeof(char) * (FILENAME_MAX + 1));

  _mode                   = mode;

  _librariesAlloc         = 0;
  _libraries              = NULL;

  _readsAlloc             = 0;
  _reads                  = NULL;

  _blobsData              = NULL;

  _blobsFilesMax          = 0;
  _blobsFiles             = NULL;

  _blobsWriter            = NULL;

  _numberOfPartitions     = 0;
  _partitionID            = 0;
  _readIDtoPartitionIdx   = NULL;
  _readIDtoPartitionID    = NULL;
  _readsPerPartition      = NULL;

  //  Save the path and name.

  if (storePath)   strncpy(_storePath, storePath, FILENAME_MAX);   //  storePath must always exist though.
  if (clonePath)   strncpy(_clonePath, clonePath, FILENAME_MAX);   //  clonePath is definitely optional.

  //  If the info file exists, load it.

  snprintf(nameI, FILENAME_MAX, "%s/info", _storePath);

  if (AS_UTL_fileExists(nameI, false, false) == true)
    AS_UTL_loadFile(nameI, &_info, 1);

  //  Check sizes are correct.

  if (_info.checkInfo() == false) {
    fprintf(stderr, "\n");
    fprintf(stderr, "ERROR:  Can't open store '%s': parameters in gkStore.H and gkRead.H are incompatible with the store.\n", _storePath);
    exit(1);
  }

  //
  //  CREATE - allocate some memory for saving libraries and reads, and create a file to dump the data into.
  //

  if (mode == gkStore_create) {
    if (partID != UINT32_MAX)
      fprintf(stderr, "gkStore()-- Illegal combination of gkStore_create with defined partID.\n"), exit(1);

    if (AS_UTL_fileExists(_storePath, true, true) == true)
      fprintf(stderr, "ERROR:  Can't create store '%s': store already exists.\n", _storePath), exit(1);

    AS_UTL_mkdir(_storePath);

    _librariesAlloc = 32;           //  _libraries and 
    _readsAlloc     = 32768;        //  _reads MUST be preallocated.

    _libraries      = new gkLibrary [_librariesAlloc];
    _reads          = new gkRead    [_readsAlloc];

    _blobsWriter    = new gkStoreBlobWriter(_storePath);

    return;
  }

  //
  //  Not creating, so the store MUST exist.  Check some other conditions too.
  //

  if (AS_UTL_fileExists(_storePath, true, false) == false)
    fprintf(stderr, "gkStore()--  failed to open '%s' for read-only access: store doesn't exist.\n", _storePath), exit(1);

  if ((mode == gkStore_extend) &&
      (partID != UINT32_MAX))
    fprintf(stderr, "gkStore()-- Illegal combination of gkStore_extend with defined partID.\n"), exit(1);

  //
  //  EXTEND - just load the metadata, allocate some stuff, and return.
  //

  if (mode == gkStore_extend) {
    gkStore_loadMetadata();

    _blobsFilesMax = omp_get_max_threads();
    _blobsFiles    = new gkStoreBlobReader [_blobsFilesMax];

    _blobsWriter   = new gkStoreBlobWriter(_storePath);

    return;
  }

  //
  //  BUILDING PARTITIONS - load metadata and return.
  //

  if (mode == gkStore_buildPart) {
    gkStore_loadMetadata();

    _blobsFilesMax = omp_get_max_threads();
    _blobsFiles    = new gkStoreBlobReader [_blobsFilesMax];

    return;
  }

  //
  //  READ ONLY non-partitioned - just load the metadata and return.
  //

  if (partID == UINT32_MAX) {       //  READ ONLY, non-partitioned (also for creating partitions)
    gkStore_loadMetadata();

    _blobsFilesMax = omp_get_max_threads();
    _blobsFiles    = new gkStoreBlobReader [_blobsFilesMax];

    return;
  }

  //
  //  READ ONLY partitioned.  A whole lotta work to do.
  //

  snprintf(nameI, FILENAME_MAX, "%s/partitions/map", _storePath);

  FILE *F = AS_UTL_openInputFile(nameI);

  AS_UTL_safeRead(F, &_numberOfPartitions, "gkStore::_numberOfPartitions", sizeof(uint32), 1);

  _partitionID            = partID;
  _readsPerPartition      = new uint32 [_numberOfPartitions   + 1];  //  No zeroth element in any of these
  _readIDtoPartitionID    = new uint32 [gkStore_getNumReads() + 1];
  _readIDtoPartitionIdx   = new uint32 [gkStore_getNumReads() + 1];

  AS_UTL_safeRead(F, _readsPerPartition,    "gkStore::_readsPerPartition",    sizeof(uint32), _numberOfPartitions   + 1);
  AS_UTL_safeRead(F, _readIDtoPartitionID,  "gkStore::_readIDtoPartitionID",  sizeof(uint32), gkStore_getNumReads() + 1);
  AS_UTL_safeRead(F, _readIDtoPartitionIdx, "gkStore::_readIDtoPartitionIdx", sizeof(uint32), gkStore_getNumReads() + 1);

  AS_UTL_closeFile(F, nameI);

  //  Load the rest of the data, just suck in entire files.

  snprintf(nameL, FILENAME_MAX, "%s/libraries", _storePath);
  snprintf(nameR, FILENAME_MAX, "%s/partitions/reads.%04" F_U32P, _storePath, partID);
  snprintf(nameB, FILENAME_MAX, "%s/partitions/blobs.%04" F_U32P, _storePath, partID);

  _librariesAlloc = _info.gkInfo_numLibraries() + 1;
  _readsAlloc     = _readsPerPartition[partID];

  uint64 bs       = AS_UTL_sizeOfFile(nameB);

  _libraries = new gkLibrary [_librariesAlloc];
  _reads     = new gkRead    [_readsAlloc];
  _blobsData = new uint8     [bs];

  AS_UTL_loadFile(nameL, _libraries, _librariesAlloc);
  AS_UTL_loadFile(nameR, _reads,     _readsAlloc);
  AS_UTL_loadFile(nameB, _blobsData,  bs);
}






gkStore::~gkStore() {
  char    No[FILENAME_MAX+1];
  char    Nn[FILENAME_MAX+1];
  uint32  V = 1;

  //  Save original metadata.

  if (_mode == gkStore_extend) {
    snprintf(No, FILENAME_MAX, "%s/version.%03" F_U32P, _storePath, V);
    while (AS_UTL_fileExists(No) == true) {
      V++;
      snprintf(No, FILENAME_MAX, "%s/version.%03" F_U32P, _storePath, V);
    }

    AS_UTL_mkdir(No);

    snprintf(No, FILENAME_MAX, "%s/libraries", _storePath);
    snprintf(Nn, FILENAME_MAX, "%s/version.%03" F_U32P "/libraries", _storePath, V);
    AS_UTL_rename(No, Nn);

    snprintf(No, FILENAME_MAX, "%s/reads", _storePath);
    snprintf(Nn, FILENAME_MAX, "%s/version.%03" F_U32P "/reads", _storePath, V);
    AS_UTL_rename(No, Nn);

    snprintf(No, FILENAME_MAX, "%s/info", _storePath);
    snprintf(Nn, FILENAME_MAX, "%s/version.%03" F_U32P "/info", _storePath, V);
    AS_UTL_rename(No, Nn);

    snprintf(No, FILENAME_MAX, "%s/info.txt", _storePath);
    snprintf(Nn, FILENAME_MAX, "%s/version.%03" F_U32P "/info.txt", _storePath, V);
    AS_UTL_rename(No, Nn);
  }

  //  Recount.

  if ((_mode == gkStore_create) ||
      (_mode == gkStore_extend)) {
    _info.recountReads(_reads);
  }
#if 0
    _info.numRawReads = _info.numCorrectedReads = _info.numTrimmedReads = 0;
    _info.numRawBases = _info.numCorrectedBases = _info.numTrimmedBases = 0;


    for (uint32 ii=0; ii<_info.numReads + 1; ii++) {
      if (_reads[ii]._rseqLen > 0) {
        _info.numRawReads++;
        _info.numRawBases += _reads[ii]._rseqLen;
      }

      if (_reads[ii]._cseqLen > 0) {
        _info.numCorrectedReads++;
        _info.numCorrectedBases += _reads[ii]._cseqLen;
      }

      if (_reads[ii]._clearBgn < _reads[ii]._clearEnd) {
        _info.numTrimmedReads++;
        _info.numTrimmedBases += _reads[ii]._clearEnd - _reads[ii]._clearBgn;
      }
    }
  }
#endif

  //  Write updated metadata.

  if ((_mode == gkStore_create) ||
      (_mode == gkStore_extend)) {
    AS_UTL_saveFile(_storePath, '/', "libraries", _libraries, gkStore_getNumLibraries() + 1);
    AS_UTL_saveFile(_storePath, '/', "reads",     _reads,     gkStore_getNumReads()     + 1);
    AS_UTL_saveFile(_storePath, '/', "info",     &_info,                                  1);

    FILE *F = AS_UTL_openOutputFile(_storePath, '/', "info.txt");   //  Used by Canu/Gatekeeper.pm
    _info.writeInfoAsText(F);                                       //  Do not remove!
    AS_UTL_closeFile(F, _storePath, '/', "info.txt");
  }

  //  Write original metadata to the clone.

  if (_mode == gkStore_buildPart) {
    AS_UTL_saveFile(_clonePath, '/', "libraries", _libraries, gkStore_getNumLibraries() + 1);
    AS_UTL_saveFile(_clonePath, '/', "info",     &_info,                                  1);

    FILE *F = AS_UTL_openOutputFile(_clonePath, '/', "info.txt");   //  Used by Canu/Gatekeeper.pm
    _info.writeInfoAsText(F);                                       //  Do not remove!
    AS_UTL_closeFile(F, _clonePath, '/', "info.txt");
  }

  //  Clean up.

  delete [] _libraries;
  delete [] _reads;
  delete [] _blobsData;
  delete [] _blobsFiles;

  delete    _blobsWriter;

  delete [] _readIDtoPartitionIdx;
  delete [] _readIDtoPartitionID;
  delete [] _readsPerPartition;
};



gkStore *
gkStore::gkStore_open(char const *path, gkStore_mode mode, uint32 partID) {

  //  If an instance exists, return it, otherwise, make a new one.

#pragma omp critical
  {
    if (_instance != NULL) {
      _instanceCount++;
    } else {
      _instance      = new gkStore(path, NULL, mode, partID);
      _instanceCount = 1;
    }
  }

  return(_instance);
}



gkStore *
gkStore::gkStore_open(char const *storePath, char const *clonePath) {

  //  Only one instance can be opened at a time.

#pragma omp critical
  {
    assert(_instance == NULL);

    _instance      = new gkStore(storePath, clonePath, gkStore_buildPart, UINT32_MAX);
    _instanceCount = 1;
  }

  return(_instance);
}



void
gkStore::gkStore_close(void) {

#pragma omp critical
  {
    _instanceCount--;

    if (_instanceCount == 0) {
      delete _instance;
      _instance = NULL;
    }
  }
}



void
gkStore::gkStore_delete(void) {
  char path[FILENAME_MAX+1];

  gkStore_deletePartitions();

  snprintf(path, FILENAME_MAX, "%s/info",      _storePath);  AS_UTL_unlink(path);
  snprintf(path, FILENAME_MAX, "%s/libraries", _storePath);  AS_UTL_unlink(path);
  snprintf(path, FILENAME_MAX, "%s/reads",     _storePath);  AS_UTL_unlink(path);
  snprintf(path, FILENAME_MAX, "%s/blobs",     _storePath);  AS_UTL_unlink(path);

  AS_UTL_rmdir(_storePath);
}



void
gkStore::gkStore_deletePartitions(void) {
  char path[FILENAME_MAX+1];

  snprintf(path, FILENAME_MAX, "%s/partitions/map", _storePath);

  if (AS_UTL_fileExists(path, false, false) == false)
    return;

  //  How many partitions?

  FILE *F = AS_UTL_openInputFile(path);

  AS_UTL_safeRead(F, &_numberOfPartitions, "gkStore_deletePartitions::numberOfPartitions", sizeof(uint32), 1);

  AS_UTL_closeFile(F, path);

  //  Yay!  Delete!

  AS_UTL_unlink(path);

  for (uint32 ii=0; ii<_numberOfPartitions; ii++) {
    snprintf(path, FILENAME_MAX, "%s/partitions/reads.%04u", _storePath, ii+1);  AS_UTL_unlink(path);
    snprintf(path, FILENAME_MAX, "%s/partitions/blobs.%04u", _storePath, ii+1);  AS_UTL_unlink(path);
  }

  //  And the directory.

  snprintf(path, FILENAME_MAX, "%s/partitions", _storePath);

  AS_UTL_rmdir(path);
}


