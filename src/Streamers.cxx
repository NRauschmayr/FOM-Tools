/*
 *  Copyright (c) CERN 2015
 *
 *  Authors:
 *      Nathalie Rauschmayr <nathalie.rauschmayr_ at _ cern _dot_ ch>
 *      Sami Kama <sami.kama_ at _ cern _dot_ ch>
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

#include "FOMTools/Streamers.hpp"
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <cstring>
#include <ios>
#include <iostream>
#include <exception>
#include <stdexcept>
#include <sys/time.h>
#include <chrono> //to get utc

#define handle_error(msg)				\
  do { perror(msg); exit(EXIT_FAILURE); } while (0)
static const uintptr_t pageMask=(sysconf(_SC_PAGE_SIZE) - 1);


void timespec_add (struct timespec *left, struct timespec *right) {
    // long tmp1 = long(left->tv_sec + right->tv_sec);
    // long tmp2 = long(left->tv_nsec + right->tv_nsec);
  
    // if (tmp2 >= 1000000000)
    //  {  
    //     ++tmp1;
    //     tmp2 -= 1000000000;
    //  }
  left->tv_sec += right->tv_sec;
  left->tv_nsec+= right->tv_nsec;
  if (left->tv_nsec>1000000000l){
    left->tv_sec++;
    left->tv_nsec-= 1000000000l;
  }
}

static int timespec_sub (struct timespec *diff, long xsec, long xnsec, long ysec, long ynsec)
{
  diff->tv_sec=xsec-ysec;
  diff->tv_nsec=xnsec-ynsec;
  if(diff->tv_nsec<0){
    diff->tv_sec--;
    diff->tv_nsec+=1000000000;
  }
}


FOM_mallocHook::MemRecord::MemRecord(void* r){
  m_h.tstart=0;
  m_h.treturn=0;
  m_h.tend=0;
  m_h.allocType=0;
  m_h.addr=0;
  m_h.size=0;
  m_h.count=0;
  m_stacks=0;
  m_overlap=MemRecord::Undefined;
  if(r){
    auto hh=(FOM_mallocHook::header*)r;
    m_h=*hh;
    m_stacks=(FOM_mallocHook::index_t*)(hh+1);
  }
}

FOM_mallocHook::MemRecord::OVERLAP_TYPE FOM_mallocHook::MemRecord::getOverlap()const {return m_overlap;};

void FOM_mallocHook::MemRecord::setOverlap(FOM_mallocHook::MemRecord::OVERLAP_TYPE o){m_overlap=o;}

FOM_mallocHook::MemRecord::~MemRecord(){};

const FOM_mallocHook::header* const FOM_mallocHook::MemRecord::getHeader() const{
  return &m_h;
  }

const FOM_mallocHook::index_t* const FOM_mallocHook::MemRecord::getStacks(int *count) const {
  *count=m_h.count;
  return m_stacks;
}

uintptr_t FOM_mallocHook::MemRecord::getFirstPage()const {
  return (m_h.addr&(~pageMask));
}

uintptr_t FOM_mallocHook::MemRecord::getLastPage()const {
  return (m_h.addr+m_h.size)|pageMask;
}

uint64_t FOM_mallocHook::MemRecord::getTStart()const{
  return m_h.tstart;
}

uint64_t FOM_mallocHook::MemRecord::getTReturn()const{
  return m_h.treturn;
}

uint64_t FOM_mallocHook::MemRecord::getTEnd()const{
  return m_h.tend;
}


char FOM_mallocHook::MemRecord::getAllocType() const{
  return m_h.allocType; 
}

uintptr_t FOM_mallocHook::MemRecord::getAddr() const{
  return m_h.addr;
}

size_t FOM_mallocHook::MemRecord::getSize() const{
  return m_h.size;

}

std::vector<FOM_mallocHook::index_t> FOM_mallocHook::MemRecord::getStacks() const{
  if((m_h.count==0)||(m_stacks==0)){
    return std::vector<FOM_mallocHook::index_t>();
  }
  return std::vector<FOM_mallocHook::index_t> (m_stacks,m_stacks+m_h.count);
}

/* READER CLASS
 */

FOM_mallocHook::Reader::Reader(std::string fileName):m_fileHandle(-1),
						     m_fileLength(0),m_fileName(fileName),m_fileBegin(0),m_fileStats(0),m_fileOpened(false)
   {
  if(m_fileName.empty())throw std::ios_base::failure("File name is empty");
  int inpFile=open(m_fileName.c_str(),O_RDONLY);
  if(inpFile==-1){
    std::cerr<<"Input file \""<<m_fileName<<"\" does not exist"<<std::endl;
    char buff[2048];
    throw std::ios_base::failure(std::string(strerror_r(errno,buff,2048)));
  }
  m_fileHandle=inpFile;
  struct stat sinp;
  if(fstat(m_fileHandle,&sinp)==-1){
    char buff[2048];
    throw std::ios_base::failure(std::string(strerror_r(errno,buff,2048)));    
  }
  if(sinp.st_size<sizeof(FOM_mallocHook::header)){
    throw std::length_error("Corrupt file. File is too short");
  }
  m_fileLength=sinp.st_size;
  m_fileOpened=true;
  //size_t nrecords=0;
  char buff[2050];
  m_fileStats=new FOM_mallocHook::FileStats();
  //std::cout << m_fileStats << " " << m_fileHandle << std::endl;
  m_fileStats->read(m_fileHandle,false);
  off_t hdrOff=::lseek64(m_fileHandle,0,SEEK_CUR);
  ::lseek64(m_fileHandle,0,SEEK_SET);
  m_fileBegin=mmap64(0,sinp.st_size,PROT_READ,MAP_PRIVATE,inpFile,0);
  if(m_fileBegin==MAP_FAILED){
    throw std::ios_base::failure(std::string(strerror_r(errno,buff,2048))+"failed to mmap "+m_fileName);        
  }
  std::cout<<"Starting to scan the file. File should contain "<<
    m_fileStats->getNumRecords()<<" entries"<<std::endl;

  void* fileEnd=(char*)m_fileBegin+sinp.st_size;
  FOM_mallocHook::header *h=(FOM_mallocHook::header*)(((uintptr_t)m_fileBegin)+hdrOff);
  while ((void*)h<fileEnd){
    m_records.emplace_back((void*)h);
    const auto hdr=m_records.back().getHeader();
    h=(FOM_mallocHook::header*)(((FOM_mallocHook::index_t*)(h+1))+h->count);
  }
  std::cout<<"Found "<<m_records.size()<<" records"<<std::endl;
}

const FOM_mallocHook::FileStats* FOM_mallocHook::Reader::getFileStats()const{
  return m_fileStats;
}

FOM_mallocHook::Reader::~Reader(){
  if(m_fileOpened){
    munmap(m_fileBegin,m_fileLength);
    close(m_fileHandle);
    m_records.clear();
  }
}

const FOM_mallocHook::MemRecord& FOM_mallocHook::Reader::at(size_t t){return m_records.at(t);}

size_t FOM_mallocHook::Reader::size(){return m_records.size();}

/* WRITER CLASS
 */

FOM_mallocHook::Writer::Writer(std::string fileName,int comp,size_t bsize):m_fileName(fileName),
									   m_nRecords(0),
									   m_maxDepth(0),
									   m_fileHandle(-1),
									   m_fileOpened(false),
									   m_bucket(0),
									   m_cBucket(0),
									   m_compress(comp),
									   m_bucketSize(bsize),
									   m_stats(0){
  if(m_fileName.empty())throw std::ios_base::failure("File name is empty");
  int outFile=open(m_fileName.c_str(),O_WRONLY|O_CREAT|O_TRUNC,(S_IRWXU^S_IXUSR)|(S_IRWXG^S_IXGRP)|(S_IROTH));
  //std::cerr<<__PRETTY_FUNCTION__<<m_fileName<<" @fd="<<outFile<<" pid= "<<getpid()<<std::endl;
  if(outFile==-1){
    std::cerr<<"Can't open out file \""<<m_fileName<<"\""<<std::endl;
    char buff[2048];
    throw std::ios_base::failure(std::string(strerror_r(errno,buff,2048)));
  }
  m_fileHandle=outFile;
  m_stats=new FileStats();
  m_stats->setVersion(20000);
  m_stats->setPid(getpid());
  struct timespec tp;
  int rc=clock_gettime(CLOCK_MONOTONIC,&tp);
  m_stats->setStartTime(tp.tv_sec*1000000000l+tp.tv_nsec);
  m_stats->setStartTimeUTC(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  size_t len=2048;
  char *buff=new char[len];
  if(!parseCmdline(buff,&len)){
    throw std::ios_base::failure(std::string("Parsing process commandline failed! ")+
				 std::string(strerror_r(errno,buff,2048)));
  }
  m_stats->setCompression(comp);
  m_stats->setBucketSize(bsize);
  m_stats->setCmdLine(buff,len);
  //for(auto &cl:m_stats->getCmdLine()){std::cerr<<cl<<" ";}std::cerr<<std::endl;
  m_stats->write(m_fileHandle,false);
  m_fileOpened=true;
  m_nRecords=0;
  m_maxDepth=0;
  delete[] buff;
}


bool FOM_mallocHook::Writer::closeFile(bool flush){
  if(m_fileOpened){
    //std::cerr<<__PRETTY_FUNCTION__<<fsync(m_fileHandle)<<" "<<m_fileName<<" @fd= "<<m_fileHandle<<" currOffset="<<::lseek64(m_fileHandle,0,SEEK_CUR)<<" pid= "<<getpid()<<std::endl;    
    if(flush){
      if(m_stats){
	//std::cerr<<"Nrecords= "<<m_nRecords<<" max depth="<<m_maxDepth<<std::endl;
	m_stats->setNumRecords(m_nRecords);
	m_stats->setStackDepthLimit(m_maxDepth);
	m_stats->write(m_fileHandle,false);
      }
    }
    fsync(m_fileHandle);
    //std::cerr<<__PRETTY_FUNCTION__<<fsync(m_fileHandle)<<" "<<m_fileName<<" @fd= "<<m_fileHandle<<" pid= "<<getpid()<<std::endl;
    close(m_fileHandle);
    //std::cerr<<__PRETTY_FUNCTION__<<close(m_fileHandle)<<" "<<m_fileName<<" @fd= "<<m_fileHandle<<" pid= "<<getpid()<<std::endl;
    delete m_stats;
    m_stats=0;
    m_fileOpened=false;
    return true;
  }else{
    return false;
  }
}

bool FOM_mallocHook::Writer::reopenFile(bool seekEnd){
  if(m_fileOpened){
    return false;
  }
  if(m_fileName.empty())throw std::ios_base::failure("File name is empty");
  int outFile=open(m_fileName.c_str(),O_RDONLY,(S_IRWXU^S_IXUSR)|(S_IRWXG^S_IXGRP)|(S_IROTH));
    if(outFile==-1){
    std::cerr<<"Can't open out file \""<<m_fileName<<"\""<<std::endl;
    char buff[2048];
    throw std::ios_base::failure(std::string("Openning file failed ")+std::string(strerror_r(errno,buff,2048)));
  }
  m_stats=new FOM_mallocHook::FileStats();
  m_stats->read(outFile,false);
  fsync(outFile);
  close(outFile);
  outFile=open(m_fileName.c_str(),O_WRONLY,(S_IRWXU^S_IXUSR)|(S_IRWXG^S_IXGRP)|(S_IROTH));
  if(outFile==-1){
    std::cerr<<"Can't open out file \""<<m_fileName<<"\""<<std::endl;
    char buff[2048];
    throw std::ios_base::failure(std::string("Openning file failed ")+std::string(strerror_r(errno,buff,2048)));
  }
  fsync(outFile);
  m_fileHandle=outFile;
  m_fileOpened=true;
  if(seekEnd){
    ::lseek64(outFile,0,SEEK_END);
  }
  return true;
}

FOM_mallocHook::Writer::~Writer(){
  if(m_fileOpened){
    if(m_stats){
      //std::cerr<<__PRETTY_FUNCTION__<<" Nrecords= "<<m_nRecords<<" max depth="<<m_maxDepth<<" @pid="<<getpid()<<std::endl;
      m_stats->setNumRecords(m_nRecords);
      m_stats->setStackDepthLimit(m_maxDepth);
      //m_stats->print();
      m_stats->write(m_fileHandle,false);
    }
    fsync(m_fileHandle);
    close(m_fileHandle);
  }
  delete m_stats;
  m_stats=0;
}

bool FOM_mallocHook::Writer::parseCmdline(char* b,size_t *len){
  auto pid=getpid();
  char buff[201];
  snprintf(buff,200,"/proc/%d/cmdline",pid);
  int cmdHandle=::open(buff,O_RDONLY);
  ssize_t cmdLen=::read(cmdHandle,b,*len);
  if(cmdLen<0){
    close(cmdHandle);
    throw std::ios_base::failure(std::string(" parse commandline failed ")+std::string(strerror_r(errno,buff,200)));
  }else if(cmdLen==0){
    close(cmdHandle);
    return false;
  }else if(cmdLen==*len){//buffer may not be enough!
    std::cerr<<"Warning commandline may be incomplete! buffer filled up completely"<<std::endl;
    *len=cmdLen;
  }else{
    *len=cmdLen;
  }
  close(cmdHandle);
  return true;
}

time_t FOM_mallocHook::Writer::getProcessStartTime(){
  auto pid=getpid();
  char fbuff[64];
  snprintf(fbuff,63,"/proc/%d/stat",pid);
  char inpBuff[4096];
  for(int i=0;i<4096;i++)inpBuff[i]=0;
  unsigned long long mstart=0;
  int uptimeFD=::open("/proc/uptime",O_RDONLY);
  ssize_t uptimeSize=::read(uptimeFD,inpBuff,4096);
  if(uptimeSize>0){
    sscanf(inpBuff,"%llu",&mstart);
  }
  ::close(uptimeFD);
  int statFD=::open(fbuff,O_RDONLY);
  ssize_t statSize=::read(statFD,inpBuff,4096);
  unsigned long long jiffies=0;
  if(statSize>0){
    if(statSize==4096){
      std::cerr<<"Can't read stat file, it is too big"<<std::endl;
    }
    char* l=inpBuff+statSize;
    while(l>inpBuff){
      if(*l==')')break;
      l--;
    }
    l+=4;
    int count=0;
    while(count<18){
      if(*l==' ')count++;
      l++;
    }
    sscanf(l,"%llu",&jiffies);
  }
  ::close(statFD);
  return (time_t)(mstart+(jiffies/sysconf(_SC_CLK_TCK)));
}

bool FOM_mallocHook::Writer::writeRecord(const MemRecord&r){
  const auto  hdr=r.getHeader();
  int nStacks=0;
  auto stIds=r.getStacks(&nStacks);
  m_nRecords++;
  if(m_maxDepth<nStacks)m_maxDepth=nStacks;
  if(write(m_fileHandle,hdr,sizeof(*hdr))!=sizeof(*hdr)){
    char buff[2048];
    throw std::ios_base::failure(std::string(" WriteRecord1 ")+std::string(strerror_r(errno,buff,2048)));
  }
  if(write(m_fileHandle,stIds,sizeof(*stIds)*nStacks)!=sizeof(*stIds)*nStacks){
    char buff[2048];
    throw std::ios_base::failure(std::string(" WriteRecord1 ")+std::string(strerror_r(errno,buff,2048)));
  }
  return true;
}

bool FOM_mallocHook::Writer::writeRecord(const RecordIndex&r){
  const auto  hdr=r.getHeader();
  int nStacks=0;
  auto stIds=r.getStacks(&nStacks);
  m_nRecords++;
  if(m_maxDepth<nStacks)m_maxDepth=nStacks;
  if(write(m_fileHandle,hdr,sizeof(*hdr))!=sizeof(*hdr)){
    char buff[2048];
    throw std::ios_base::failure(std::string(" WriteRecord2 ")+std::string(strerror_r(errno,buff,2048)));
  }
  if(write(m_fileHandle,stIds,sizeof(*stIds)*nStacks)!=sizeof(*stIds)*nStacks){
    char buff[2048];
    throw std::ios_base::failure(std::string(" WriteRecord2 ")+std::string(strerror_r(errno,buff,2048)));
  }
  return true;
}

bool FOM_mallocHook::Writer::writeRecord(const void *r){
  const auto  hdr=(FOM_mallocHook::header*)r;
  int nStacks=hdr->count;
  auto stIds=((FOM_mallocHook::index_t*)(hdr+1));
  m_nRecords++;
  if(m_maxDepth<nStacks)m_maxDepth=nStacks;
  if(write(m_fileHandle,hdr,sizeof(*hdr))!=sizeof(*hdr)){
    char buff[2048];
    throw std::ios_base::failure(std::string(" WriteRecord3 ")+std::string(strerror_r(errno,buff,2048)));
  }
  if(write(m_fileHandle,stIds,sizeof(*stIds)*nStacks)!=sizeof(*stIds)*nStacks){
    char buff[2048];
    throw std::ios_base::failure(std::string(" WriteRecord3 ")+std::string(strerror_r(errno,buff,2048)));
  }
  return true;
}

FOM_mallocHook::FileStats::FileStats(){
  m_hdr=new FileStats::fileHdr();
  strncpy(m_hdr->key,"FOM",4);
  m_hdr->ToolVersion=-1;
  m_hdr->Compression=0;  
  m_hdr->NumRecords=0;
  m_hdr->MaxStacks=0;
  m_hdr->BucketSize=0;
  m_hdr->NumBuckets=0;
  m_hdr->Pid=0;
  m_hdr->CmdLength=0;
  m_hdr->CmdLine=0;
  
}

FOM_mallocHook::FileStats::~FileStats(){
  delete[] m_hdr->CmdLine;
  delete m_hdr;
}

int FOM_mallocHook::FileStats::getVersion()const{
  return m_hdr->ToolVersion;
}

size_t FOM_mallocHook::FileStats::getNumRecords()const{
  return m_hdr->NumRecords;
}

size_t FOM_mallocHook::FileStats::getMaxStackLen()const{
  return m_hdr->MaxStacks;
}


std::vector<std::string> FOM_mallocHook::FileStats::getCmdLine()const{
  std::vector<std::string> cmds;
  if(m_hdr->CmdLength){
    char* c=m_hdr->CmdLine;
    char *cmdEnd=m_hdr->CmdLine+m_hdr->CmdLength;
    while(c<cmdEnd){
      std::string s(c);
      c+=s.length()+1;
      cmds.push_back(s);
    }
  }
  return cmds;
}

uint64_t FOM_mallocHook::FileStats::getStartTime()const{
  return m_hdr->StartTime;
}
uint64_t FOM_mallocHook::FileStats::getStartUTC()const{
  return m_hdr->StartTimeUtc;
}

uint32_t FOM_mallocHook::FileStats::getPid()const{
  return m_hdr->Pid;
}

int FOM_mallocHook::FileStats::getCompression()const{
  return m_hdr->Compression;
}

size_t   FOM_mallocHook::FileStats::getBucketSize()const{
  return m_hdr->BucketSize;
}
size_t   FOM_mallocHook::FileStats::getNumBuckets()const{
  return m_hdr->NumBuckets;
}


void FOM_mallocHook::FileStats::setVersion(int ver){
  m_hdr->ToolVersion=ver;
}

void FOM_mallocHook::FileStats::setNumRecords(size_t n){
  m_hdr->NumRecords=n;
}

void FOM_mallocHook::FileStats::setStackDepthLimit(size_t l){
  m_hdr->MaxStacks=l;
}


void FOM_mallocHook::FileStats::setCmdLine(char* cmd,size_t len){
  delete m_hdr->CmdLine;
  m_hdr->CmdLength=len;
  if(len){
    m_hdr->CmdLine=new char[len+1];
    ::memcpy(m_hdr->CmdLine,cmd,len);
    m_hdr->CmdLine[len]='\0';
  }else{
    m_hdr->CmdLine=0; 
  }
}

void FOM_mallocHook::FileStats::setStartTime(uint64_t t){
  m_hdr->StartTime=t;
}

void FOM_mallocHook::FileStats::setStartTimeUTC(uint64_t t){
  m_hdr->StartTimeUtc=t;
}

void FOM_mallocHook::FileStats::setPid(uint32_t p){
  m_hdr->Pid=p;
}

void FOM_mallocHook::FileStats::setCompression(int comp){
  m_hdr->Compression=comp;
}

void FOM_mallocHook::FileStats::setBucketSize(size_t n){
  m_hdr->BucketSize=n;
}

void FOM_mallocHook::FileStats::setNumBuckets(size_t n){
  m_hdr->NumBuckets=n;
}

int FOM_mallocHook::FileStats::read(int fd,bool keepOffset){
#define READS(X) if(::read(fd, (X) , sizeof(X))<0){char buff[2048];	\
  throw std::ios_base::failure(std::string("Parsing header failed ")+	\
			       std::string(strerror_r(errno,buff,2048)));} \
  //std::cerr<<"Read "<<#X<<" = "<<X<<" sizeof="<<sizeof(X)<<" pid="<<getpid()<<" fd="<<fd<<" offs="<<::lseek64(fd,0,SEEK_CUR)<<std::endl;
#define READ(X) if(::read(fd, &(X) , sizeof(X))<0){char buff[2048];	\
  throw std::ios_base::failure(std::string("Parsing header failed ")+	\
			       std::string(strerror_r(errno,buff,2048)));} \
  //std::cerr<<"Read "<<#X<<" = "<<X<<" sizeof="<<sizeof(X)<<" pid="<<getpid()<<" fd="<<fd<<" offs="<<::lseek64(fd,0,SEEK_CUR)<<std::endl;
  if(fd<0){
    throw std::ios_base::failure("Invalid file descriptor in read()");
  }
  auto currPos=::lseek64(fd,0,SEEK_CUR);
  if(currPos==-1){
    char buff[2048];
    throw std::ios_base::failure(std::string("Finding file offset failed ")+
				 std::string(strerror_r(errno,buff,2048)));
  }
  auto fBegin=::lseek64(fd,0,SEEK_SET);
  if(fBegin==-1){
    char buff[2048];
    throw std::ios_base::failure(std::string("File seek failed ")+
				 std::string(strerror_r(errno,buff,2048)));
  }
  
  // if(::read(fd,m_hdr->key,sizeof(m_hdr->key))<0){
  //   char buff[2048];
  //   throw std::ios_base::failure(std::string("Parsing header failed")+
  // 				 std::string(strerror_r(errno,buff,2048)));
  // }
  // // std::cout<<"Key="<<m_hdr->key<<sizeof(m_hdr->key)<<std::endl;
  // if(::read(fd,&(m_hdr->ToolVersion),sizeof(m_hdr->ToolVersion))<0){
  //   char buff[2048];
  //   throw std::ios_base::failure(std::string("Parsing header failed")+
  // 				 std::string(strerror_r(errno,buff,2048)));
  // }
  // if(::read(fd,&(m_hdr->Compression),sizeof(m_hdr->Compression))<0){
  //   char buff[2048];
  //   throw std::ios_base::failure(std::string("Parsing header failed")+
  // 				 std::string(strerror_r(errno,buff,2048)));
  // }
  //   // std::cout<<"ToolVersion="<<m_hdr->ToolVersion<<" "<<sizeof(m_hdr->ToolVersion)<<std::endl;
  // if(::read(fd,&(m_hdr->NumRecords),sizeof(m_hdr->NumRecords))<0){
  //   char buff[2048];
  //   throw std::ios_base::failure(std::string("Parsing header failed")+
  // 				 std::string(strerror_r(errno,buff,2048)));
  // }
  // // std::cout<<"numRecords="<<m_hdr->numRecords<<" "<<sizeof(m_hdr->numRecords)<<std::endl;  
  // if(::read(fd,&(m_hdr->MaxStacks),sizeof(m_hdr->MaxStacks))<0){
  //   char buff[2048];
  //   throw std::ios_base::failure(std::string("Parsing header failed")+
  // 				 std::string(strerror_r(errno,buff,2048)));
  // }
  // if(::read(fd,&(m_hdr->BucketSize),sizeof(m_hdr->BucketSize))<0){
  //   char buff[2048];
  //   throw std::ios_base::failure(std::string("Parsing header failed")+
  // 				 std::string(strerror_r(errno,buff,2048)));
  // }
  // if(::read(fd,&(m_hdr->NumBuckets),sizeof(m_hdr->NumBuckets))<0){
  //   char buff[2048];
  //   throw std::ios_base::failure(std::string("Parsing header failed")+
  // 				 std::string(strerror_r(errno,buff,2048)));
  // }
  // // std::cout<<"maxStacks="<<m_hdr->maxStacks<<" "<<sizeof(m_hdr->maxStacks)<<std::endl;
  // if(::read(fd,&(m_hdr->Pid),sizeof(m_hdr->Pid))<0){
  //   char buff[2048];
  //   throw std::ios_base::failure(std::string("Parsing header failed")+
  // 				 std::string(strerror_r(errno,buff,2048)));
  // }
  // // std::cout<<"pid="<<m_hdr->pid<<" "<<sizeof(m_hdr->pid)<<std::endl;
  // if(::read(fd,&(m_hdr->StartTime),sizeof(m_hdr->StartTime))<0){
  //   char buff[2048];
  //   throw std::ios_base::failure(std::string("Parsing header failed")+
  // 				 std::string(strerror_r(errno,buff,2048)));
  // }
  // if(::read(fd,&(m_hdr->StartTimeUtc),sizeof(m_hdr->StartTimeUtc))<0){
  //   char buff[2048];
  //   throw std::ios_base::failure(std::string("Parsing header failed")+
  // 				 std::string(strerror_r(errno,buff,2048)));
  // }
  // // std::cout<<"Start time="<<m_hdr->startTime<<" "<<sizeof(m_hdr->startTime)<<std::endl;
  // if(::read(fd,&(m_hdr->CmdLength),sizeof(m_hdr->CmdLength))<0){
  //   char buff[2048];
  //   throw std::ios_base::failure(std::string("Parsing header failed")+
  // 				 std::string(strerror_r(errno,buff,2048)));
  // }
  // // std::cerr<<"Cmdlength="<<m_hdr->cmdLength<<" "<<sizeof(m_hdr->cmdLength)<<std::endl;
  // //std::cerr<<"cmdLength="<<m_hdr->cmdLength<<std::endl;
  READS(m_hdr->key);
  READ(m_hdr->ToolVersion);
  READ(m_hdr->Compression);
  READ(m_hdr->NumRecords);
  READ(m_hdr->MaxStacks);
  READ(m_hdr->BucketSize);
  READ(m_hdr->NumBuckets);
  READ(m_hdr->Pid);
  READ(m_hdr->StartTime);
  READ(m_hdr->StartTimeUtc);
  READ(m_hdr->CmdLength);
  delete[] m_hdr->CmdLine;
  m_hdr->CmdLine=0;
  if(m_hdr->CmdLength){
    m_hdr->CmdLine=new char[m_hdr->CmdLength+1];
    if(::read(fd,m_hdr->CmdLine,m_hdr->CmdLength)<0){
      char buff[2048];
      throw std::ios_base::failure(std::string("Parsing header failed ")+
				   std::string(strerror_r(errno,buff,2048)));
    }
    // std::cout<<"cmdLine="<<m_hdr->cmdLine<<std::endl;
  }
  if(keepOffset){
    if(::lseek64(fd,currPos,SEEK_SET)==-1){
      char buff[2048];
      throw std::ios_base::failure(std::string("Seek failed ")+
				   std::string(strerror_r(errno,buff,2048)));
    }
    // std::cout<<"File Offset "<<currPos<<std::endl;
  }
  return 0;
}

int FOM_mallocHook::FileStats::write(int fd,bool keepOffset)const{
#define WRITES(X) if(::write(fd,X,sizeof(X))!=sizeof(X)){char buff[2048]; \
  throw std::ios_base::failure(std::string("Writing header failed with ")+	\
			       std::string(strerror_r(errno,buff,2048)));} \
  //std::cerr<<"Write "<<#X<<" = "<<X<<" sizeof="<<sizeof(X)<<" pid="<<getpid()<<" fd="<<fd<<" offs="<<::lseek64(fd,0,SEEK_CUR)<<std::endl;
#define WRITE(X) if(::write(fd,&(X),sizeof(X))!=sizeof(X)){char buff[2048]; \
  throw std::ios_base::failure(std::string("Writing header failed with ")+	\
			       std::string(strerror_r(errno,buff,2048)));} \
  //std::cerr<<"Write "<<#X<<" = "<<X<<" sizeof="<<sizeof(X)<<" pid="<<getpid()<<" fd="<<fd<<" offs="<<::lseek64(fd,0,SEEK_CUR)<<std::endl;

  if(fd<0){
    throw std::ios_base::failure("Invalid file descriptor in write()");
  }
  auto currPos=::lseek64(fd,0,SEEK_CUR);
  if(currPos==-1){
    char buff[2048];
    throw std::ios_base::failure(std::string("Finding file offset failed ")+
				 std::string(strerror_r(errno,buff,2048)));
  }
  // std::cout<<"File Offset "<<currPos<<std::endl;
  if(::lseek64(fd,0,SEEK_SET)==-1){
    char buff[2048];
    throw std::ios_base::failure(std::string("File seek failed ")+
				 std::string(strerror_r(errno,buff,2048)));
  }
  // if(::write(fd,m_hdr->key,sizeof(m_hdr->key))<0){
  //   char buff[2048];
  //   throw std::ios_base::failure(std::string("Writing header failed")+
  // 				 std::string(strerror_r(errno,buff,2048)));
  // }
  // // std::cout<<"Key="<<m_hdr->key<<sizeof(m_hdr->key)<<std::endl;
  // if(::write(fd,&(m_hdr->ToolVersion),sizeof(m_hdr->ToolVersion))<0){
  //   char buff[2048];
  //   throw std::ios_base::failure(std::string("Writing header failed")+
  // 				 std::string(strerror_r(errno,buff,2048)));
  // }
  // // std::cout<<"ToolVersion="<<m_hdr->ToolVersion<<" "<<sizeof(m_hdr->ToolVersion)<<std::endl;
  // if(::write(fd,&(m_hdr->Compression),sizeof(m_hdr->Compression))<0){
  //   char buff[2048];
  //   throw std::ios_base::failure(std::string("Writing header failed")+
  // 				 std::string(strerror_r(errno,buff,2048)));
  // }
  // if(::write(fd,&(m_hdr->NumRecords),sizeof(m_hdr->NumRecords))<0){
  //   char buff[2048];
  //   throw std::ios_base::failure(std::string("Writing header failed")+
  // 				 std::string(strerror_r(errno,buff,2048)));
  // }
  // // std::cout<<"numRecords="<<m_hdr->numRecords<<" "<<sizeof(m_hdr->numRecords)<<std::endl;  
  // if(::write(fd,&(m_hdr->MaxStacks),sizeof(m_hdr->MaxStacks))<0){
  //   char buff[2048];
  //   throw std::ios_base::failure(std::string("Writing header failed")+
  // 				 std::string(strerror_r(errno,buff,2048)));
  // }
  // if(::write(fd,&(m_hdr->BucketSize),sizeof(m_hdr->BucketSize))<0){
  //   char buff[2048];
  //   throw std::ios_base::failure(std::string("Writing header failed")+
  // 				 std::string(strerror_r(errno,buff,2048)));
  // }
  // if(::write(fd,&(m_hdr->NumBuckets),sizeof(m_hdr->NumBuckets))<0){
  //   char buff[2048];
  //   throw std::ios_base::failure(std::string("Writing header failed")+
  // 				 std::string(strerror_r(errno,buff,2048)));
  // }
  // // std::cout<<"maxStacks="<<m_hdr->maxStacks<<" "<<sizeof(m_hdr->maxStacks)<<std::endl;
  // if(::write(fd,&(m_hdr->Pid),sizeof(m_hdr->Pid))<0){
  //   char buff[2048];
  //   throw std::ios_base::failure(std::string("Writing header failed")+
  // 				 std::string(strerror_r(errno,buff,2048)));
  // }
  // // std::cout<<"pid="<<m_hdr->pid<<" "<<sizeof(m_hdr->pid)<<std::endl;
  // if(::write(fd,&(m_hdr->StartTime),sizeof(m_hdr->StartTime))<0){
  //   char buff[2048];
  //   throw std::ios_base::failure(std::string("Writing header failed")+
  // 				 std::string(strerror_r(errno,buff,2048)));
  // }
  // if(::write(fd,&(m_hdr->StartTimeUtc),sizeof(m_hdr->StartTimeUtc))<0){
  //   char buff[2048];
  //   throw std::ios_base::failure(std::string("Writing header failed")+
  // 				 std::string(strerror_r(errno,buff,2048)));
  // }
  // // std::cout<<"Start time="<<m_hdr->startTime<<" "<<sizeof(m_hdr->startTime)<<std::endl;
  // if(::write(fd,&(m_hdr->CmdLength),sizeof(m_hdr->CmdLength))<0){
  //   char buff[2048];
  //   throw std::ios_base::failure(std::string("Writing header failed")+
  // 				 std::string(strerror_r(errno,buff,2048)));
  // }
  // std::cerr<<"Cmdlength="<<m_hdr->cmdLength<<" "<<sizeof(m_hdr->cmdLength)<<std::endl;
  WRITES(m_hdr->key);
  WRITE(m_hdr->ToolVersion);
  WRITE(m_hdr->Compression);
  WRITE(m_hdr->NumRecords);
  WRITE(m_hdr->MaxStacks);
  WRITE(m_hdr->BucketSize);
  WRITE(m_hdr->NumBuckets);
  WRITE(m_hdr->Pid);
  WRITE(m_hdr->StartTime);
  WRITE(m_hdr->StartTimeUtc);
  WRITE(m_hdr->CmdLength);

  if(m_hdr->CmdLength){
    if(::write(fd,m_hdr->CmdLine,m_hdr->CmdLength)<0){
      char buff[2048];
      throw std::ios_base::failure(std::string("Writing header failed with ")+
				   std::string(strerror_r(errno,buff,2048)));
    } 
    // std::cout<<"cmdLine="<<m_hdr->cmdLine<<std::endl;
  }
  if(keepOffset){
    if(::lseek64(fd,currPos,SEEK_SET)==-1){
      char buff[2048];
      throw std::ios_base::failure(std::string("Seek failed ")+
				   std::string(strerror_r(errno,buff,2048)));
    }
    // std::cout<<"File Offset "<<currPos<<std::endl;
  }
  return 0;
}

int FOM_mallocHook::FileStats::read(std::istream &in){
  return 0;
}

int FOM_mallocHook::FileStats::write(std::ostream &out)const{
  return 0;
}

std::ostream& FOM_mallocHook::FileStats::print(std::ostream &out)const{
    out<<"Key              = "<<m_hdr->key<<std::endl;
    out<<"Tool Version     = "<<m_hdr->ToolVersion<<std::endl;
    out<<"Compression      = "<<m_hdr->Compression<<std::endl;
    out<<"Num Records      = "<<m_hdr->NumRecords<<std::endl;
    out<<"Max Stack Depth  = "<<m_hdr->MaxStacks<<std::endl;
    out<<"Bucket Size      = "<<m_hdr->BucketSize<<std::endl;
    out<<"Num Buckets      = "<<m_hdr->NumBuckets<<std::endl;
    out<<"PID              = "<<m_hdr->Pid<<std::endl;
    out<<"Start time       = "<<m_hdr->StartTime<<std::endl;
    out<<"Start time UTC   = "<<m_hdr->StartTimeUtc<<std::endl;
    out<<"Command Line     = "<<std::endl;
    auto cmdline=getCmdLine();
    for(size_t t=0;t<cmdline.size();t++){
      out<<"  arg["<<t<<"]   = "<<cmdline.at(t)<<std::endl;
    }
    return out;
}
/* 
INDEXING READER
*/

FOM_mallocHook::IndexingReader::IndexingReader(std::string fileName,uint indexPeriod):m_fileHandle(-1),
										      m_fileLength(0),m_fileName(fileName),
										      m_fileBegin(0),m_fileStats(0),m_fileOpened(false),
										      m_period(indexPeriod),m_remainder(0),
										      m_lastIndex(0),m_numRecords(0),
										      m_lastHdr(0)
{
  if(m_fileName.empty())throw std::ios_base::failure("File name is empty ");
  int inpFile=open(m_fileName.c_str(),O_RDONLY);
  if(inpFile==-1){
    std::cerr<<"Input file \""<<m_fileName<<"\" does not exist"<<std::endl;
    char buff[2048];
    throw std::ios_base::failure(std::string(strerror_r(errno,buff,2048)));
  }
  m_fileHandle=inpFile;
  struct stat sinp;
  if(fstat(m_fileHandle,&sinp)==-1){
    char buff[2048];
    throw std::ios_base::failure(std::string(strerror_r(errno,buff,2048)));    
  }
  if(sinp.st_size<sizeof(FOM_mallocHook::header)){
    throw std::length_error("Corrupt file. File is too short");
  }
  m_fileLength=sinp.st_size;
  m_fileOpened=true;
  //size_t nrecords=0;
  char buff[2050];
  m_fileStats=new FOM_mallocHook::FileStats();
  //std::cout << m_fileStats << " " << m_fileHandle << std::endl;
  m_fileStats->read(m_fileHandle,false);
  off_t hdrOff=::lseek64(m_fileHandle,0,SEEK_CUR);
  ::lseek64(m_fileHandle,0,SEEK_SET);
  m_fileBegin=mmap64(0,sinp.st_size,PROT_READ,MAP_PRIVATE,inpFile,0);
  if(m_fileBegin==MAP_FAILED){
    throw std::ios_base::failure(std::string(strerror_r(errno,buff,2048))+"failed to mmap "+m_fileName);        
  }
  
  std::cout<<"Starting to scan the file. File should contain "<<
    m_fileStats->getNumRecords()<<" entries"<<std::endl;
  void* fileEnd=(char*)m_fileBegin+sinp.st_size;
  FOM_mallocHook::header *h=(FOM_mallocHook::header*)(((uintptr_t)m_fileBegin)+hdrOff);
  m_records.reserve(m_fileStats->getNumRecords());
  if(m_period<1)m_period=100;
  size_t count=0;
  m_lastHdr=h;
  while ((void*)h<fileEnd){
    if((count%m_period)==0)m_records.emplace_back(h);
    h=(FOM_mallocHook::header*)(((FOM_mallocHook::index_t*)(h+1))+h->count);
    count++;
  }
  m_numRecords=count;
  m_remainder=((count-1)%m_period);
  std::cout<<"Counted "<<count<<" records. Created "<<m_records.size()<<" index points. Remaining "<< m_remainder<<" records"<<std::endl;
}

const FOM_mallocHook::FileStats* FOM_mallocHook::IndexingReader::getFileStats()const{
  return m_fileStats;
}

FOM_mallocHook::IndexingReader::~IndexingReader(){
  if(m_fileOpened){
    munmap(m_fileBegin,m_fileLength);
    close(m_fileHandle);
    m_records.clear();
  }
}

const FOM_mallocHook::RecordIndex FOM_mallocHook::IndexingReader::at(size_t t){
  if(t>=m_numRecords){
    char bu[500];
    snprintf(bu,500,"Asked for an index larger than number of records! t=%ld size=%ld",t,m_numRecords);
    throw std::length_error(bu);
  }
  size_t bucket=t/m_period;
  size_t offset=t-(bucket*m_period);
  if(offset==0){return m_records.at(bucket);}
  size_t d=t-m_lastIndex;
  if((d>0) &&(d<offset)){
    auto h=m_lastHdr;
    for(int i=0;i<d;i++){
      h=(FOM_mallocHook::header*)(((FOM_mallocHook::index_t*)(h+1))+h->count);
    }
    m_lastHdr=h;
  }else{
    auto h=m_records.at(bucket).getHeader();
    for(int i=0;i<offset;i++){
      h=(FOM_mallocHook::header*)(((FOM_mallocHook::index_t*)(h+1))+h->count);
    }
    m_lastHdr=h;
  }
  m_lastIndex=t;
  return FOM_mallocHook::RecordIndex(m_lastHdr);
}

size_t FOM_mallocHook::IndexingReader::size(){
  return (((m_records.size()-1)*m_period)+m_remainder+1);
}
size_t FOM_mallocHook::IndexingReader::indexedSize(){
  return m_records.size();
}

/*
// Record Index
*/
FOM_mallocHook::RecordIndex::RecordIndex(const FOM_mallocHook::header* h):m_h(0),m_overlap(MemRecord::Undefined){
  if(h){
    m_h=h;
  }
}
FOM_mallocHook::MemRecord::OVERLAP_TYPE FOM_mallocHook::RecordIndex::getOverlap()const {return m_overlap;};

void FOM_mallocHook::RecordIndex::setOverlap(FOM_mallocHook::MemRecord::OVERLAP_TYPE o){m_overlap=o;}

FOM_mallocHook::RecordIndex::~RecordIndex(){};

const FOM_mallocHook::header* const FOM_mallocHook::RecordIndex::getHeader() const{
  return m_h;
}

const FOM_mallocHook::index_t* const FOM_mallocHook::RecordIndex::getStacks(int *count) const {
  if(m_h){
    *count=m_h->count;
    return (FOM_mallocHook::index_t*)(m_h+1);
  }
  *count=0;
  return 0;

}

uintptr_t FOM_mallocHook::RecordIndex::getFirstPage()const {
  return (m_h->addr&(~pageMask));
}

uintptr_t FOM_mallocHook::RecordIndex::getLastPage()const {
  return (m_h->addr+m_h->size)|pageMask;
}

uint64_t FOM_mallocHook::RecordIndex::getTStart()const{
  return m_h->tstart;
}

uint64_t FOM_mallocHook::RecordIndex::getTReturn()const{
  return m_h->treturn;
}

uint64_t FOM_mallocHook::RecordIndex::getTEnd()const{
  return m_h->tend;
}

char FOM_mallocHook::RecordIndex::getAllocType() const{
  return m_h->allocType; 
}

uintptr_t FOM_mallocHook::RecordIndex::getAddr() const{
  return m_h->addr;
}

size_t FOM_mallocHook::RecordIndex::getSize() const{
  return m_h->size;

}

std::vector<FOM_mallocHook::index_t> FOM_mallocHook::RecordIndex::getStacks() const{
  if((m_h==0)||(m_h->count==0)){
    return std::vector<FOM_mallocHook::index_t>();
  }
  return std::vector<FOM_mallocHook::index_t> ((FOM_mallocHook::index_t*)(m_h+1),((FOM_mallocHook::index_t*)(m_h+1))+m_h->count);
}

/*
FullRecord
*/

FOM_mallocHook::FullRecord::FullRecord(const FOM_mallocHook::header* h):m_h(0),m_overlap(MemRecord::Undefined){
  if(h){//make a local copy
    m_h=(FOM_mallocHook::header*)new char[sizeof(FOM_mallocHook::header)+(h->count*sizeof(FOM_mallocHook::index_t))];
    *m_h=*h;
    auto dst=((FOM_mallocHook::index_t*)(m_h+1));
    auto src=((FOM_mallocHook::index_t*)(h+1));
    for(int i=0;i<h->count;i++){
      src[i]=dst[i];
    }
  }
}

FOM_mallocHook::FullRecord::FullRecord(const FullRecord& rhs):m_h(0),m_overlap(rhs.m_overlap){
  if(rhs.m_h){//make a local copy
    m_h=(FOM_mallocHook::header*)new char[sizeof(FOM_mallocHook::header)+(rhs.m_h->count*sizeof(FOM_mallocHook::index_t))];
    *m_h=*rhs.m_h;
    auto dst=((FOM_mallocHook::index_t*)(m_h+1));
    auto src=((FOM_mallocHook::index_t*)(rhs.m_h+1));
    for(int i=0;i<rhs.m_h->count;i++){
      src[i]=dst[i];
    }
  }
}

FOM_mallocHook::MemRecord::OVERLAP_TYPE FOM_mallocHook::FullRecord::getOverlap()const {return m_overlap;};

void FOM_mallocHook::FullRecord::setOverlap(FOM_mallocHook::MemRecord::OVERLAP_TYPE o){m_overlap=o;}

FOM_mallocHook::FullRecord::~FullRecord(){delete[] (char*)m_h;};

const FOM_mallocHook::header* const FOM_mallocHook::FullRecord::getHeader() const{
  return m_h;
}

const FOM_mallocHook::index_t* const FOM_mallocHook::FullRecord::getStacks(int *count) const {
  if(m_h){
    *count=m_h->count;
    return (FOM_mallocHook::index_t*)(m_h+1);
  }
  *count=0;
  return 0;

}

uintptr_t FOM_mallocHook::FullRecord::getFirstPage()const {
  return (m_h->addr&(~pageMask));
}

uintptr_t FOM_mallocHook::FullRecord::getLastPage()const {
  return (m_h->addr+m_h->size)|pageMask;
}
uint64_t FOM_mallocHook::FullRecord::getTStart()const{
  return m_h->tstart;
}

uint64_t FOM_mallocHook::FullRecord::getTReturn()const{
  return m_h->treturn;
}

uint64_t FOM_mallocHook::FullRecord::getTEnd()const{
  return m_h->tend;
}


char FOM_mallocHook::FullRecord::getAllocType() const{
  return m_h->allocType; 
}

uintptr_t FOM_mallocHook::FullRecord::getAddr() const{
  return m_h->addr;
}

size_t FOM_mallocHook::FullRecord::getSize() const{
  return m_h->size;

}

std::vector<FOM_mallocHook::index_t> FOM_mallocHook::FullRecord::getStacks() const{
  if((m_h==0)||(m_h->count==0)){
    return std::vector<FOM_mallocHook::index_t>();
  }
  return std::vector<FOM_mallocHook::index_t> ((FOM_mallocHook::index_t*)(m_h+1),((FOM_mallocHook::index_t*)(m_h+1))+m_h->count);
}
