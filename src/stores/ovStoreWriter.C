
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
 *    src/stores/ovStore.C
 *
 *  Modifications by:
 *
 *    Brian P. Walenz beginning on 2016-OCT-28
 *      are a 'United States Government Work', and
 *      are released in the public domain
 *
 *  File 'README.licenses' in the root directory of this distribution contains
 *  full conditions and disclaimers for each license.
 */

#include "ovStore.H"


void
checkAndSaveName(char *storePath, const char *path) {
  if (path == NULL)
    fprintf(stderr, "ovStoreWriter::ovStoreWriter()-- ERROR: no name supplied.\n"), exit(1);

  if ((path[0] == '-') &&
      (path[1] == 0))
    fprintf(stderr, "ovStoreWriter::ovStoreWriter()-- ERROR: name cannot be '-' (stdin).\n"), exit(1);

  memset(storePath, 0, FILENAME_MAX);
  strncpy(storePath, path, FILENAME_MAX-1);
}



ovStoreWriter::~ovStoreWriter() {

  //  Write the last index element, filling in gaps.

  if (_offt._numOlaps > 0) {
    for (; _offm._a_iid < _offt._a_iid; _offm._a_iid++) {
      _offm._fileno   = _offt._fileno;
      _offm._offset   = _offt._offset;
      _offm._numOlaps = 0;

      AS_UTL_safeWrite(_offtFile, &_offm, "ovStore::~ovStore::offm", sizeof(ovStoreOfft), 1);
    }

    AS_UTL_safeWrite(_offtFile, &_offt, "ovStore::~ovStore::offt", sizeof(ovStoreOfft), 1);
  }

  AS_UTL_closeFile(_offtFile);

  //  Update the on-disk info with the results and real magic number

  _info.save(_storePath, _currentFileIndex);

  //  Update our copy of the histogram from the last open file, and close it.

  if (_bof)
    _bof->transferHistogram(_histogram);
  delete _bof;

  //  Save the histogram data.

  if (_histogram)
    _histogram->saveData(_storePath);
  delete _histogram;

  //  Report a nice success message.

  fprintf(stderr, "Created ovStore '%s' with " F_U64 " overlaps for reads from " F_U32 " to " F_U32 ".\n",
          _storePath, _info.numOverlaps(), _info.smallestID(), _info.largestID());
}




////////////////////////////////////////
//
//  SEQUENTIAL STORE - only two functions.
//

ovStoreWriter::ovStoreWriter(const char *path, gkStore *gkp) {
  char name[FILENAME_MAX];

  checkAndSaveName(_storePath, path);

  //  Fail if this is a valid ovStore.

  if (_info.test(_storePath) == true)
    fprintf(stderr, "ERROR:  '%s' is a valid ovStore; cannot create a new one.\n", _storePath), exit(1);

  //  Create the new store

  AS_UTL_mkdir(_storePath);

  _info.clear();
  _info.save(_storePath);

  _gkp       = gkp;

  _offtFile  = NULL;
  _offt.clear();
  _offm.clear();

  _evaluesMap         = NULL;
  _evalues            = NULL;

  _overlapsThisFile  = 0;
  _currentFileIndex  = 0;
  _bof               = NULL;

  //  This is used by the sequential store build, so we want to collect stats.

  _histogram = new ovStoreHistogram(_gkp, ovFileNormalWrite);

  //  Open the index file.

  snprintf(name, FILENAME_MAX, "%s/index", _storePath);

  errno = 0;
  _offtFile = fopen(name, "w");
  if (errno)
    fprintf(stderr, "AS_OVS_createOverlapStore()-- failed to open offset file '%s': %s\n", name, strerror(errno)), exit(1);

  _overlapsThisFile    = 0;
  _overlapsThisFileMax = 0;  //  1024 * 1024 * 1024 / _bof->recordSize();   --  needs a valid _bof, dang.
  _currentFileIndex    = 0;
  _bof                 = NULL;

  _fileLimit           = 0;  //  Used in the parallel store, not here.
  _fileID              = 0;
  _jobIdxMax           = 0;
}



void
ovStoreWriter::writeOverlap(ovOverlap *overlap) {
  char            name[FILENAME_MAX];

  //  Make sure overlaps are sorted, failing if not.

  if (_offt._a_iid > overlap->a_iid) {
    fprintf(stderr, "LAST:  a:" F_U32 "\n", _offt._a_iid);
    fprintf(stderr, "THIS:  a:" F_U32 " b:" F_U32 "\n", overlap->a_iid, overlap->b_iid);
  }
  assert(_offt._a_iid <= overlap->a_iid);

  //  If we don't have an output file yet, or the current file is
  //  too big, open a new file.

  if ((_bof) && (_overlapsThisFile >= _overlapsThisFileMax)) {
    _bof->transferHistogram(_histogram);

    delete _bof;

    _bof                 = NULL;
    _overlapsThisFile    = 0;
    _overlapsThisFileMax = 0;
  }

  if (_bof == NULL) {
    char  name[FILENAME_MAX];

    snprintf(name, FILENAME_MAX, "%s/%04d", _storePath, ++_currentFileIndex);

    _bof                 = new ovFile(_gkp, name, ovFileNormalWrite);
    _overlapsThisFile    = 0;
    _overlapsThisFileMax = 1024 * 1024 * 1024 / _bof->recordSize();
  }

  //  Put the index to disk, filling any gaps

  if ((_offt._numOlaps != 0) &&
      (_offt._a_iid != overlap->a_iid)) {

    while (_offm._a_iid < _offt._a_iid) {
      _offm._fileno    = _offt._fileno;
      _offm._offset    = _offt._offset;
      _offm._overlapID = _offt._overlapID;  //  Not needed, but makes life easier

      AS_UTL_safeWrite(_offtFile, &_offm, "ovStore::writeOverlap::offset", sizeof(ovStoreOfft), 1);

      _offm._a_iid++;
    }

    _offm._a_iid++;  //  One more, since this iid is not missing -- we write it next!

    AS_UTL_safeWrite(_offtFile, &_offt, "AS_OVS_writeOverlapToStore offset", sizeof(ovStoreOfft), 1);

    _offt._numOlaps = 0;  //  Reset; this new id has no overlaps yet.
  }

  //  Update the index if this is the first overlap for this a_iid

  if (_offt._numOlaps == 0) {
    _offt._a_iid     = overlap->a_iid;
    _offt._fileno    = _currentFileIndex;
    _offt._offset    = _overlapsThisFile;
    _offt._overlapID = _info.numOverlaps();
  }

  _bof->writeOverlap(overlap);

  _offt._numOlaps++;
  _info.addOverlap(overlap->a_iid);
  _overlapsThisFile++;
}



////////////////////////////////////////
//
//  PARALLEL STORE - many functions, all the rest.
//

ovStoreWriter::ovStoreWriter(const char *path, gkStore *gkp, uint32 fileLimit, uint32 fileID, uint32 jobIdxMax) {

  checkAndSaveName(_storePath, path);

  _gkp                 = gkp;

  _offtFile            = NULL;
  _evaluesMap          = NULL;
  _evalues             = NULL;

  _overlapsThisFile    = 0;
  _overlapsThisFileMax = 0;
  _currentFileIndex    = 0;
  _bof                 = NULL;

  _histogram           = NULL;

  _fileLimit           = fileLimit;
  _fileID              = fileID;
  _jobIdxMax           = jobIdxMax;
};



uint64
ovStoreWriter::loadBucketSizes(uint64 *bucketSizes) {
  char      namz[FILENAME_MAX];
  char      name[FILENAME_MAX];

  uint64   *sliceSizes = new uint64 [_fileLimit + 1];  //  For each overlap job, number of overlaps per bucket
  uint64    totOvl     = 0;

  for (uint32 i=0; i<=_jobIdxMax; i++) {
    bucketSizes[i] = 0;

    snprintf(name, FILENAME_MAX, "%s/bucket%04d/slice%04d",    _storePath, i, _fileID);
    snprintf(namz, FILENAME_MAX, "%s/bucket%04d/slice%04d.gz", _storePath, i, _fileID);

    //  If no file, there are no overlaps.  Skip loading the bucketSizes file.
    //  With snappy compression, we expect the file to be not gzip compressed, but will happily
    //  accept a gzipped file.

    if ((AS_UTL_fileExists(name, FALSE, FALSE) == false) &&
        (AS_UTL_fileExists(namz, FALSE, FALSE) == false))
      continue;

    snprintf(name, FILENAME_MAX, "%s/bucket%04d/sliceSizes", _storePath, i);

    FILE  *F  = AS_UTL_openInputFile(name);
    uint64 nr = AS_UTL_safeRead(F, sliceSizes, "sliceSizes", sizeof(uint64), _fileLimit + 1);

    AS_UTL_closeFile(F, name);

    if (nr != _fileLimit + 1) {
      fprintf(stderr, "ERROR: short read on '%s'.\n", name);
      fprintf(stderr, "ERROR: read " F_U64 " sizes insteadof " F_U32 ".\n", nr, _fileLimit + 1);
    }
    assert(nr == _fileLimit + 1);

    fprintf(stderr, "  found %10" F_U64P " overlaps in '%s'.\n", sliceSizes[_fileID], name);

    bucketSizes[i] = sliceSizes[_fileID];
    totOvl        += sliceSizes[_fileID];
  }

  delete [] sliceSizes;

  return(totOvl);
}



void
ovStoreWriter::loadOverlapsFromSlice(uint32 slice, uint64 expectedLen, ovOverlap *ovls, uint64& ovlsLen) {
  char name[FILENAME_MAX];

  if (expectedLen == 0)
    return;

  snprintf(name, FILENAME_MAX, "%s/bucket%04d/slice%04d", _storePath, slice, _fileID);

  if (AS_UTL_fileExists(name, FALSE, FALSE) == false) {
    snprintf(name, FILENAME_MAX, "%s/bucket%04d/slice%04d.gz", _storePath, slice, _fileID);

    if (AS_UTL_fileExists(name, FALSE, FALSE) == false)
      fprintf(stderr, "ERROR: " F_U64 " overlaps claim to exist in bucket '%s', but file not found.\n",
              expectedLen, name);
  }

  fprintf(stderr, "  loading %10" F_U64P " overlaps from '%s'.\n", expectedLen, name);

  ovFile   *bof = new ovFile(_gkp, name, ovFileFull);
  uint64    num = 0;

  while (bof->readOverlap(ovls + ovlsLen)) {
    ovlsLen++;
    num++;
  }

  if (num != expectedLen)
    fprintf(stderr, "ERROR: expected " F_U64 " overlaps, found " F_U64 " overlaps.\n", expectedLen, num);
  assert(num == expectedLen);

  delete bof;
}



//  Write a block of sorted overlaps into a single file, with index and info.

void
ovStoreWriter::writeOverlaps(ovOverlap  *ovls,
                             uint64      ovlsLen) {

  uint32         currentFileIndex = _fileID;

  ovStoreInfo    info;

  info.clear();

  ovStoreOfft    offt;
  ovStoreOfft    offm;

  offt._a_iid     = offm._a_iid     = ovls[0].a_iid;
  offt._fileno    = offm._fileno    = _fileID;
  offt._offset    = offm._offset    = 0;
  offt._numOlaps  = offm._numOlaps  = 0;
  offt._overlapID = offm._overlapID = 0;

  //  Create the output file

  char  offtName[FILENAME_MAX+1];

  snprintf(offtName, FILENAME_MAX, "%s/%04d", _storePath, _fileID);
  ovFile *bof = new ovFile(_gkp, offtName, ovFileNormalWrite);

  //  Create the index file

  snprintf(offtName, FILENAME_MAX, "%s/%04d.index", _storePath, _fileID);
  FILE *offtFile = AS_UTL_openOutputFile(offtName);

  //  Dump the overlaps

  for (uint64 i=0; i<ovlsLen; i++ ) {
    bof->writeOverlap(ovls + i);

    if (offt._a_iid > ovls[i].a_iid) {
      fprintf(stderr, "LAST:  a:" F_U32 "\n", offt._a_iid);
      fprintf(stderr, "THIS:  a:" F_U32 " b:" F_U32 "\n", ovls[i].a_iid, ovls[i].b_iid);
    }
    assert(offt._a_iid <= ovls[i].a_iid);

    //  Put the index to disk, filling any gaps

    if ((offt._numOlaps != 0) && (offt._a_iid != ovls[i].a_iid)) {
      while (offm._a_iid < offt._a_iid) {
        offm._fileno     = offt._fileno;
        offm._offset     = offt._offset;
        offm._overlapID  = offt._overlapID;  //  Not needed, but makes life easier
        offm._numOlaps   = 0;

        AS_UTL_safeWrite(offtFile, &offm, "AS_OVS_writeOverlapToStore offt", sizeof(ovStoreOfft), 1);
        offm._a_iid++;
      }

      //  One more, since this iid is not offm -- we write it next!
      offm._a_iid++;

      AS_UTL_safeWrite(offtFile, &offt, "AS_OVS_writeOverlapToStore offt", sizeof(ovStoreOfft), 1);

      offt._overlapID += offt._numOlaps;  //  The next block of overlaps starts with this ID
      offt._numOlaps   = 0;               //  The next block has no overlaps yet.
    }

    //  Update the index if this is the first overlap for this a_iid

    if (offt._numOlaps == 0) {
      offt._a_iid   = ovls[i].a_iid;
      offt._fileno  = currentFileIndex;
      offt._offset  = info.numOverlaps();
    }

    offt._numOlaps++;

    info.addOverlap(ovls[i].a_iid);
  }

  //  Close the output file.

  delete bof;

  //  Write the final (empty) index entries.

  while (offm._a_iid < offt._a_iid) {
    offm._fileno     = offt._fileno;
    offm._offset     = offt._offset;
    offm._overlapID  = offt._overlapID;  //  Not needed, but makes life easier
    offm._numOlaps   = 0;

    AS_UTL_safeWrite(offtFile, &offm, "AS_OVS_writeOverlapToStore offt", sizeof(ovStoreOfft), 1);
    offm._a_iid++;
  }

  //  And the final (real) index entry.  We could, but don't need to, update overlapID with the
  //  number of overlaps in this block.

  AS_UTL_safeWrite(offtFile, &offt, "AS_OVS_writeOverlapToStore offt", sizeof(ovStoreOfft), 1);

  AS_UTL_closeFile(offtFile, offtName);

  //  Write the info, and some stats for the user.

  info.save(_storePath, _fileID, true);

  fprintf(stderr, "  created '%s/%04d' with " F_U64 " overlaps for reads " F_U32 " to " F_U32 ".\n",
          _storePath, _fileID, info.numOverlaps(), info.smallestID(), info.largestID());
}








//  For the parallel sort, merge index and info files into one, clean up the intermediates.

void
ovStoreWriter::mergeInfoFiles(void) {
  ovStoreInfo    infopiece;
  ovStoreInfo    info;

  info.clear();

  ovStoreOfft offm;

  offm._a_iid     = 0;
  offm._fileno    = 1;
  offm._offset    = 0;
  offm._numOlaps  = 0;
  offm._overlapID = 0;

  //  Open the new master index output file

  char            name[FILENAME_MAX];

  snprintf(name, FILENAME_MAX, "%s/index", _storePath);

  errno = 0;
  FILE  *idx = fopen(name, "w");
  if (errno)
    fprintf(stderr, "ERROR: Failed to open '%s': %s\n", name, strerror(errno)), exit(1);

  //  Special case, we need an empty index for the zeroth fragment.

  AS_UTL_safeWrite(idx, &offm, "ovStore::mergeInfoFiles::offsetZero", sizeof(ovStoreOfft), 1);

  //  Sanity checking, compare the number of overlaps processed against the overlapID
  //  of each ovStoreOfft.

  uint64  totalOverlaps = 0;

  //  Process each

  for (uint32 i=1; i<=_fileLimit; i++) {
    fprintf(stderr, "Processing '%s'\n", name);

    infopiece.load(_storePath, i, true);

    if (infopiece.numOverlaps() == 0) {
      fprintf(stderr, "  No overlaps found.\n");
      continue;
    }

    //  Add empty index elements for missing overlaps

    if (info.largestID() + 1 < infopiece.smallestID())
      fprintf(stderr, "  Adding empty records for fragments " F_U32 " to " F_U32 "\n",
              info.largestID() + 1, infopiece.smallestID() - 1);

    while (info.largestID() + 1 < infopiece.smallestID()) {
      offm._a_iid     = info.largestID() + 1;
      //offm._fileno    = set below, where the recs are written to the master file
      //offm._offset    = set below, where the recs are written to the master file

      AS_UTL_safeWrite(idx, &offm, "ovStore::mergeInfoFiles::offsets", sizeof(ovStoreOfft), 1);

      info.addOverlap(offm._a_iid, 0);
    }

    //  Copy index elements for existing overlaps.  While copying, update the supposed position
    //  of any fragments with no overlaps.  Without doing this, accessing the store beginning
    //  or ending at such a fragment will fail.

    {
      snprintf(name, FILENAME_MAX, "%s/%04d.index", _storePath, i);

      FILE  *F = AS_UTL_openInputFile(name);

      uint32          recsLen = 0;
      uint32          recsMax = 1024 * 1024;
      ovStoreOfft    *recs    = new ovStoreOfft [recsMax];

      recsLen = AS_UTL_safeRead(F, recs, "ovStore::mergeInfoFiles::offsetsLoad", sizeof(ovStoreOfft), recsMax);

      if (recsLen > 0) {
        if (info.largestID() + 1 != recs[0]._a_iid)
          fprintf(stderr, "ERROR: '%s' starts with iid " F_U32 ", but store only up to " F_U32 "\n",
                  name, recs[0]._a_iid, info.largestID());
        assert(info.largestID() + 1 == recs[0]._a_iid);
      }

      while (recsLen > 0) {

        //  Update location of missing reads.

        offm._fileno     = recs[recsLen-1]._fileno;
        offm._offset     = recs[recsLen-1]._offset;

        //  Update overlapID for each record.

        for (uint32 rr=0; rr<recsLen; rr++) {
          recs[rr]._overlapID += info.numOverlaps();

          if (recs[rr]._numOlaps > 0)
            assert(recs[rr]._overlapID == totalOverlaps);

          totalOverlaps += recs[rr]._numOlaps;
        }

        //  Write the records, read next batch

        AS_UTL_safeWrite(idx, recs, "ovStore::mergeInfoFiles::offsetsWrite", sizeof(ovStoreOfft), recsLen);

        recsLen = AS_UTL_safeRead(F, recs, "ovStore::mergeInfoFiles::offsetsReLoad", sizeof(ovStoreOfft), recsMax);
      }

      delete [] recs;

      AS_UTL_closeFile(F, name);
    }

    //  Update the info block to include the overlaps we just added

    info.addOverlap(infopiece.smallestID(), 0);
    info.addOverlap(infopiece.largestID(), infopiece.numOverlaps());

    fprintf(stderr, "  Now finished with fragments " F_U32 " to " F_U32 " -- " F_U64 " overlaps.\n",
            info.smallestID(), info.largestID(), info.numOverlaps());
  }

  AS_UTL_closeFile(idx);


  //  Dump the new store info file

  info.save(_storePath, _fileLimit);

  fprintf(stderr, "Created ovStore '%s' with " F_U64 " overlaps for reads from " F_U32 " to " F_U32 ".\n",
          _storePath, info.numOverlaps(), info.smallestID(), info.largestID());
}



void
ovStoreWriter::mergeHistogram(void) {
  char               name[FILENAME_MAX];
  ovStoreHistogram  *merged = new ovStoreHistogram;
  ovStoreHistogram  *piece  = new ovStoreHistogram;

  for (uint32 i=1; i<=_fileLimit; i++) {
    snprintf(name, FILENAME_MAX, "%s/%04d", _storePath, i);

    piece->clear();
    piece->loadData(name);

    merged->add(piece);
  }

  merged->saveData(_storePath);

  delete piece;
  delete merged;
}



bool
ovStoreWriter::testIndex(bool doFixes) {

  //  Open the input index.

  char Iname[FILENAME_MAX+1];

  snprintf(Iname, FILENAME_MAX, "%s/index", _storePath);

  FILE *I = AS_UTL_openInputFile(Iname);

  //  If we're fixing, open the output index.

  char Fname[FILENAME_MAX+1];

  snprintf(Fname, FILENAME_MAX, "%s/index.fixed", _storePath);

  FILE *F = (doFixes == true) ? AS_UTL_openOutputFile(Fname) : NULL;


  ovStoreOfft  O;

  uint32  curIID = 0;
  uint32  minIID = UINT32_MAX;
  uint32  maxIID = 0;

  uint32  nErrs = 0;

  while (1 == AS_UTL_safeRead(I, &O, "offset", sizeof(ovStoreOfft), 1)) {
    bool  maxIncreases   = (maxIID < O._a_iid);
    bool  errorDecreased = ((O._a_iid < curIID));
    bool  errorGap       = ((O._a_iid > 0) && (curIID + 1 != O._a_iid));

    if (O._a_iid < minIID)
      minIID = O._a_iid;

    if (maxIncreases)
      maxIID = O._a_iid;

    if (errorDecreased)
      fprintf(stderr, "ERROR: index decreased from " F_U32 " to " F_U32 "\n", curIID, O._a_iid), nErrs++;
    else if (errorGap)
      fprintf(stderr, "ERROR: gap between " F_U32 " and " F_U32 "\n", curIID, O._a_iid), nErrs++;

    if ((maxIncreases == true) && (errorGap == false)) {
      if (doFixes)
        AS_UTL_safeWrite(F, &O, "offset", sizeof(ovStoreOfft), 1);

    } else if (O._numOlaps > 0) {
      fprintf(stderr, "ERROR: lost overlaps a_iid " F_U32 " fileno " F_U32 " offset " F_U32 " numOlaps " F_U32 "\n",
              O._a_iid, O._fileno, O._offset, O._numOlaps);
    }

    curIID = O._a_iid;
  }

  AS_UTL_closeFile(I, Iname);
  AS_UTL_closeFile(F, Fname);

  return(nErrs == 0);
}



void
ovStoreWriter::removeOverlapSlice(void) {
  char name[FILENAME_MAX];

  for (uint32 i=0; i<=_jobIdxMax; i++) {
    snprintf(name, FILENAME_MAX, "%s/bucket%04d/slice%04d.gz", _storePath, i, _fileID);    AS_UTL_unlink(name);
    snprintf(name, FILENAME_MAX, "%s/bucket%04d/slice%04d",    _storePath, i, _fileID);    AS_UTL_unlink(name);
  }
}



void
ovStoreWriter::checkSortingIsComplete(void) {
  char    nameD[FILENAME_MAX];
  char    nameF[FILENAME_MAX];
  char    nameI[FILENAME_MAX];

  uint32  failedJobs = 0;

  for (uint32 i=1; i<=_fileLimit; i++) {
    snprintf(nameD, FILENAME_MAX, "%s/%04d", _storePath, i);
    snprintf(nameF, FILENAME_MAX, "%s/%04d.info", _storePath, i);
    snprintf(nameI, FILENAME_MAX, "%s/%04d.index", _storePath, i);

    bool existD = AS_UTL_fileExists(nameD, FALSE, FALSE);
    bool existF = AS_UTL_fileExists(nameF, FALSE, FALSE);
    bool existI = AS_UTL_fileExists(nameI, FALSE, FALSE);

    if (existD && existF && existI)
      continue;

    failedJobs++;

    if (existD == false)    fprintf(stderr, "ERROR: Segment " F_U32 " data  not present (%s)\n", i, nameD);
    if (existF == false)    fprintf(stderr, "ERROR: Segment " F_U32 " info  not present (%s)\n", i, nameF);
    if (existI == false)    fprintf(stderr, "ERROR: Segment " F_U32 " index not present (%s)\n", i, nameI);
  }

  if (failedJobs > 0)
    fprintf(stderr, "ERROR: " F_U32 " segments, out of " F_U32 ", failed.\n", _fileLimit, failedJobs), exit(1);
}



void
ovStoreWriter::removeAllIntermediateFiles(void) {
  char name[FILENAME_MAX];

  //  Removing indices and histogram data is easy, beacuse we know how many there are.

  for (uint32 i=1; i<=_fileLimit; i++) {
    snprintf(name, FILENAME_MAX, "%s/%04u.index", _storePath, i);    AS_UTL_unlink(name);
    snprintf(name, FILENAME_MAX, "%s/%04u.info",  _storePath, i);    AS_UTL_unlink(name);
    snprintf(name, FILENAME_MAX, "%s/%04d",       _storePath, i);    ovStoreHistogram::removeData(name);
  }

  //  We don't know how many buckets there are, so we remove until we fail to find ten
  //  buckets in a row.

  for (uint32 missing=0, i=1; missing<10; i++, missing++) {
    snprintf(name, FILENAME_MAX, "%s/bucket%04d", _storePath, i);

    if (AS_UTL_fileExists(name, false, false) == false)
      continue;

    snprintf(name, FILENAME_MAX, "%s/bucket%04d/sliceSizes", _storePath, i);    AS_UTL_unlink(name);
    snprintf(name, FILENAME_MAX, "%s/bucket%04d",            _storePath, i);    rmdir(name);

    missing = 0;
  }
}
