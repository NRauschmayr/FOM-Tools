#include "FOMTools/Streamers.hpp"
#include "TSystem.h"
#include <vector>
#include "TFile.h"
#include "TTree.h"
#include "TBranch.h"
#include <iostream>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <getopt.h>
#include <fcntl.h>
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <deque>
#include <tuple>
#include <sstream>

class FreeRecord{
public:
  FreeRecord(size_t f,uint64_t tc,uint64_t to):filePos(f),TCorr(tc),TOff(to){};
  size_t filePos;
  uint64_t TCorr;
  uint64_t TOff;
};

size_t convert(const std::string inpName,const std::string output,
	       unsigned long LAwindow,unsigned long DWindow,
	       unsigned int bucketLength,
	       size_t mapLen
	       ){
  FOM_mallocHook::ReaderBase *rdr=0;
  int fd=open(inpName.c_str(),O_RDONLY);
  FOM_mallocHook::FileStats *fs=new FOM_mallocHook::FileStats();
  fs->read(fd,false);
  close(fd);
  std::cout<<"processing file "<<inpName<<". Header "<<std::endl;
  fs->print(std::cout);
  if(fs->getCompression()==0){
    rdr=new FOM_mallocHook::IndexingReader(inpName,bucketLength);
  }
#ifdef ZLIB_FOUND
  else if((fs->getCompression()/10000000)==_USE_ZLIB_COMPRESSION_){
    rdr=new FOM_mallocHook::ZlibReader(inpName,bucketLength);
  }
#endif
#ifdef BZip2_FOUND
  else if((fs->getCompression()/10000000)==_USE_BZLIB_COMPRESSION_){
    rdr=new FOM_mallocHook::BZip2Reader(inpName,bucketLength);
  }
#endif
  const size_t nrecords=rdr->size();
  //configuration variables

  const size_t maxLookAheadTime=1000l*1000l*1000l*LAwindow;//in nanoseconds;
  const size_t DensityWindow=1000l*DWindow;//1000000 ns->1 ms running window
  const size_t DHalf=DensityWindow/2;
  std::unordered_map<size_t,std::deque<FreeRecord>*> freeMap;

  uint64_t T0=0;
  uint64_t T1=0;
  uint64_t T2=0;
  uint64_t TCorr=0;
  uint64_t TOffset=0;
  uint64_t TStart=0;
  int64_t Variation=0;
  int64_t Locality=0;
  int64_t LifeTime=0;
  int64_t addrLast=0;
  int64_t sizeLast=0;
  uint64_t Density=0;
  size_t addr;
  size_t size;
  unsigned char alloc_type;
  std::vector<FOM_mallocHook::index_t> stacks;
  TFile f(output.c_str(),"recreate");
  TTree t("FOMRecords","");
  t.Branch("T0",&T0,"T0/l");
  t.Branch("T1",&T1,"T1/l");
  t.Branch("T2",&T2,"T2/l");
  t.Branch("TCorrected",&TCorr,"TCorr/l");
  t.Branch("Address",&addr,"Addr/l");
  t.Branch("AType",&alloc_type,"alloc_type/b");
  t.Branch("Density",&Density,"Density/l");
  t.Branch("Lifetime",&LifeTime,"Lifetime/L");
  t.Branch("Locality",&Locality,"Locality/L");
  t.Branch("Size",&size,"Size/l");
  t.Branch("Variation",&Variation,"Variation/L");
  t.Branch("Stacks",&stacks);
  size_t marker=nrecords/100;
  int ticker=0;

  auto initial=rdr->at(0);
  TStart=fs->getStartTime();
  addrLast=initial.getAddr();
  sizeLast=initial.getSize();

  size_t windowMin=0;
  size_t windowMax=0;
  uint64_t wlOffset=0;
  uint64_t whOffset=0;
  size_t lastFreeIndex=0;
  uint64_t lastTOffset=initial.getTEnd()-initial.getTStart();
  uint64_t lastTCorr=0;
  size_t skipCount=0;
  std::deque<std::deque<FreeRecord>*> queueStack;
  auto tstart=std::chrono::steady_clock::now();
  size_t tableSize=0;
  //
  // Start looping over file
  //
  std::cout<<"Starting to process the file. This will take some time"<<std::endl;
  for(size_t i=0;i<nrecords;i++){
    auto r=rdr->at(i);
    T0  =r.getTStart();
    T1  =r.getTReturn();
    T2  =r.getTEnd();
    TCorr=(T0-TStart-TOffset);
    TOffset+=(T2-T0);
    addr   =r.getAddr();
    size   =r.getSize();
    alloc_type=r.getAllocType();
    stacks =r.getStacks();
    auto rmin=rdr->at(windowMin);
    int64_t tmin=TCorr-DHalf;
    uint64_t wlT0=rmin.getTStart();
    int64_t wlTcorr=wlT0-TStart-wlOffset;
    LifeTime=-1;

    while(wlTcorr<tmin){
      wlOffset+=rmin.getTEnd()-wlT0;
      windowMin++;
      rmin=rdr->at(windowMin);
      wlT0=rmin.getTStart();
      wlTcorr=wlT0-TStart-wlOffset;
    }

    if(windowMax<nrecords){
      auto rmax=rdr->at(windowMax);
      int64_t tmax=TCorr+DHalf;
      uint64_t whT0=rmax.getTStart();
      int64_t whTcorr=whT0-TStart-whOffset;
      while(whTcorr<tmax){
	whOffset+=rmax.getTEnd()-whT0;
	windowMax++;
	if(windowMax>=nrecords)break;
	rmax=rdr->at(windowMax);
	whT0=rmax.getTStart();
	whTcorr=whT0-TStart-whOffset;
      }
    }
    Density=windowMax-windowMin-1;
    if(alloc_type!=0){
      Variation=size-sizeLast;
      sizeLast=size;
      Locality=addr-addrLast;
      addrLast=addr;
    }

    if(alloc_type!=0){
      uint64_t TMax=TCorr+maxLookAheadTime;
      if(lastTCorr<TMax){// I don't need to check if last entry in the map is already further than lookahead buffer
	auto mapit=freeMap.find(addr);
	if(mapit==freeMap.end()){//a matching free on adress was not seen previuously
	  size_t currIdx=lastFreeIndex+1;
	  if(currIdx<nrecords){
	    auto ra=rdr->at(currIdx);
	    uint64_t aoff=lastTOffset;
	    uint64_t aT0=ra.getTStart();
	    uint64_t aTCorr=aT0-TStart-lastTOffset;
	    auto raadr=ra.getAddr();
	    if(raadr==addr && ra.getAllocType()==0 ){
	      LifeTime=aTCorr-TCorr;
	      lastFreeIndex=currIdx;
	      lastTOffset=aoff+ra.getTEnd()-aT0;
	      lastTCorr=aTCorr;
	    }else{
	      std::unordered_map<size_t,std::deque<FreeRecord>*>::iterator rit;
	      if(ra.getAllocType()==0 && raadr!=0){//filter out free(0)
		rit=freeMap.find(raadr);
		if(rit!=freeMap.end()){
		  rit->second->emplace_back(currIdx,aTCorr,aoff);
		}else{
		  std::deque<FreeRecord> *dq=0;
		  if(queueStack.size()){
		    dq=queueStack.front();
		    queueStack.pop_front();
		  }else{
		    dq=new std::deque<FreeRecord>();
		  }
		  dq->emplace_back(currIdx,aTCorr,aoff);
		  tableSize++;
		  freeMap.insert({raadr,dq});
		}
	      }
	      std::deque<FreeRecord> *dq=0;
	      while((aTCorr<TMax ||(tableSize<mapLen)) && currIdx<(nrecords-1)){
		currIdx++;
		aoff+=ra.getTEnd()-aT0;
		ra=rdr->at(currIdx);
		aT0=ra.getTStart();
		aTCorr=aT0-TStart-aoff;
		raadr=ra.getAddr();
		if(ra.getAllocType()==0 && raadr!=0 ){
		  lastFreeIndex=currIdx;
		  lastTOffset=aoff+ra.getTEnd()-aT0;
		  lastTCorr=aTCorr;
		  if(raadr==addr){
		    LifeTime=aTCorr-TCorr;
		    break;
		  }
		  rit=freeMap.find(raadr);
		  if(rit==freeMap.end()){
		    if(queueStack.size()){
		      dq=queueStack.front();
		      queueStack.pop_front();
		    }else{
		      dq=new std::deque<FreeRecord>();
		    }
		    dq->emplace_back(currIdx,aTCorr,aoff);
		    freeMap.insert({raadr,dq});
		  }else{
		    rit->second->emplace_back(currIdx,aTCorr,aoff);
		  }
		  tableSize++;
		}
	      }
	      if(aTCorr>=TMax){
		skipCount++;
	      }
	    }
	  }
	}else{//found address in map
	  auto dq=mapit->second;
	  LifeTime=dq->front().TCorr-TCorr;
	  dq->pop_front();
	  tableSize--;
	  if(dq->size()==0){
	    queueStack.push_back(dq);
	    freeMap.erase(mapit);
	  }
	}
      }else{//table already contains entries further than max window
	auto rit=freeMap.find(addr);
	if(rit!=freeMap.end()){
	  auto dq=rit->second;
	  if(!dq || dq->size()==0){
	    std::cerr<<"Error queue is broken size= "<<dq->size()<<" front= "<<(void*)(&(dq->front()))<<std::endl;
	  }
	  LifeTime=dq->front().TCorr-TCorr;
	  dq->pop_front();
	  tableSize--;
	  if(dq->size()==0){
	    queueStack.push_back(dq);
	    freeMap.erase(rit);
	  }
	}else{
	  skipCount++;
	}
      }
    }
    t.Fill();    
    if(i%marker==0){
      size_t bucketCount=freeMap.bucket_count();
      float lf=freeMap.max_load_factor();
      std::cout<<ticker<<" percent complete in "
	       <<std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-tstart).count()
	       <<" s ( "<<i<<" records, "<<TCorr<<" ns in to the data collection MapSize="<<freeMap.size()
	       <<" entry count= "<<tableSize<<" skipCount="<<skipCount
	       <<" numHashBuckets="<<bucketCount<<" loadFactor="<<lf<<" )"<<std::endl;
      ticker++;
    }
  }
  t.Write();
  f.Close();
  return nrecords;
}

void printUsage(char* name){
  std::cout<<"Usage:  "<<name<<" -i <input> -o <output> "<<std::endl;
  std::cout<<"     --input  (-i)  name of a file that is created by mallochook"<<std::endl;
  std::cout<<"     --output (-o)  output file name"<<std::endl;
  std::cout<<"     --look-ahead-width (-l) depth of look ahead buffer for lifetime determination in seconds (default 200)"<<std::endl;
  std::cout<<"     --density-width (-d) Width of density window in micro-seconds (default 1000)"<<std::endl;
  std::cout<<"     --bucket-size (-b) reader bucket size in records(default 100. shorter the size higher the memory consumption faster the search)"<<std::endl;
}


int main(int argc,char* argv[]){
  std::string inpName("");
  std::string outName("");
  unsigned long lawidth(200),DWidth(1000);
  unsigned int bucketLen(100);
  size_t mapLen(100000000);
  //struct stat sinp;
  int c;
  while (1) {
    int option_index = 0;
    static struct option long_options[] = {
      {"help", 0, 0, 'h'},
      {"input", 1, 0, 'i'},
      {"output", 1, 0, 'o'},
      {"look-ahead-width", 1, 0, 'l'},
      {"density-width", 1, 0, 'd'},
      {"bucket-size", 1, 0, 'b'},
      {"map-length", 1, 0, 'm'},
      {0, 0, 0, 0}
    };
    c = getopt_long(argc, argv, "hi:o:l:d:b:m:",
		    long_options, &option_index);
    if (c == -1){
      break;
    }
    switch (c) {
    case 'h':
      printUsage(argv[0]);
      exit(EXIT_SUCCESS);
      break;
    case 'i':  {
      inpName=std::string(optarg);
      break;
    }
    case 'o':  {
      outName=std::string(optarg);
      break;
    }
    case 'l':  {
      std::stringstream iss(optarg);
      iss>>lawidth;
      break;
    }
    case 'd':  {
      std::stringstream iss(optarg);
      iss>>DWidth;
      break;
    }
    case 'b':  {
      std::stringstream iss(optarg);
      iss>>bucketLen;
      break;
    }
    case 'm':  {
      std::stringstream iss(optarg);
      iss>>mapLen;
      break;
    }
    default:
      printf("unknown parameter! getopt returned character code 0%o ??\n", c);
    }
  }
  if(optind<argc){
    while(optind<argc){
      if(inpName.empty()){
	inpName=argv[optind];
      }else if(outName.empty()){
	inpName=argv[optind];	
      }else{
	std::cerr<<"unknown parameter "<<argv[optind]<<std::endl;
      }
      optind++;
    }
  }

  if(inpName.empty()){
    std::cout<<"Input file name is needed"<<std::endl;
    printUsage(argv[0]);
    exit(EXIT_FAILURE);
  }

  if(outName.empty()){
    // std::cout<<"Output file name is needed"<<std::endl;
    // printUsage(argv[0]);
    // exit(EXIT_FAILURE);
    outName=inpName+".root";
    std::cout<<"Using outputfile "<<outName<<std::endl;
  }
  auto tstart=std::chrono::steady_clock::now();
  size_t nrecords=convert(inpName,outName,lawidth,DWidth,bucketLen,mapLen);
  printf("Processing of %ld records took %10.3fs\n",nrecords,std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-tstart).count()/1000.);
  return 0;
}

