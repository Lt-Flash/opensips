/* paced OPTIONS sender: one packet per interval (fixed or Poisson), K rotating
 * source sockets, replies collected asynchronously; latency per packet. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <math.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#define K 256
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e6+t.tv_nsec/1e3;}
static int cmp(const void*a,const void*b){double x=*(double*)a,y=*(double*)b;return x<y?-1:x>y;}
int main(int argc,char**argv){
  if(argc<5){fprintf(stderr,"usage: pace <ip> <port> <rate/s> <count> [poisson]\n");return 1;}
  const char*ip=argv[1];int port=atoi(argv[2]);double rate=atof(argv[3]);int count=atoi(argv[4]);int poisson=argc>5;
  struct sockaddr_in dst={0};dst.sin_family=AF_INET;dst.sin_port=htons(port);inet_pton(AF_INET,ip,&dst.sin_addr);
  char lip[64]="0.0.0.0";{int t=socket(AF_INET,SOCK_DGRAM,0);connect(t,(struct sockaddr*)&dst,sizeof dst);struct sockaddr_in me;socklen_t l=sizeof me;getsockname(t,(struct sockaddr*)&me,&l);inet_ntop(AF_INET,&me.sin_addr,lip,sizeof lip);close(t);}
  int fd[K],lport[K];struct pollfd pf[K];double ts[K];int pend[K];
  for(int i=0;i<K;i++){fd[i]=socket(AF_INET,SOCK_DGRAM,0);struct sockaddr_in me={0};me.sin_family=AF_INET;bind(fd[i],(struct sockaddr*)&me,sizeof me);socklen_t l=sizeof me;getsockname(fd[i],(struct sockaddr*)&me,&l);lport[i]=ntohs(me.sin_port);pf[i].fd=fd[i];pf[i].events=POLLIN;pend[i]=0;}
  double*lat=calloc(count,sizeof(double));int nl=0,sent=0,lost=0,late=0;char buf[2048],msg[512];
  srand48(12345);double next=now()+1000;double gap=1e6/rate;
  while(sent<count||nl+lost<sent){
    double t=now();
    if(sent<count&&t>=next){
      int i=sent%K;
      if(pend[i]){lost++;pend[i]=0;}
      snprintf(msg,sizeof msg,"OPTIONS sip:ping@%s:%d SIP/2.0\r\nVia: SIP/2.0/UDP %s:%d;rport;branch=z9hG4bKp%d\r\nFrom: <sip:p@x>;tag=%d\r\nTo: <sip:ping@%s>\r\nCall-ID: pace%d\r\nCSeq: 1 OPTIONS\r\nMax-Forwards: 70\r\nContent-Length: 0\r\n\r\n",ip,port,lip,lport[i],sent,sent,ip,sent);
      sendto(fd[i],msg,strlen(msg),0,(struct sockaddr*)&dst,sizeof dst);ts[i]=now();pend[i]=1;sent++;
      if(t-next>gap)late++;
      next+=poisson?-log(1.0-drand48())*gap:gap;
    }
    int tmo=sent<count?0:50;
    int r=poll(pf,K,tmo);
    if(r>0){for(int i=0;i<K;i++) if(pf[i].revents&POLLIN){double tr=now();recv(fd[i],buf,sizeof buf,0);if(pend[i]){lat[nl++]=tr-ts[i];pend[i]=0;}pf[i].revents=0;}}
    else if(sent>=count){ /* drain timeout */ for(int i=0;i<K;i++) if(pend[i]){lost++;pend[i]=0;} }
    if(sent<count&&next-now()>200){struct timespec s={0,(long)((next-now()-100)*1000)};nanosleep(&s,NULL);}
  }
  if(nl){qsort(lat,nl,sizeof(double),cmp);
  printf("pace %s rate %.0f/s sent %d replies %d lost %d late_sends %d | latency us: p50 %.0f p90 %.0f p99 %.0f p999 %.0f max %.0f\n",poisson?"poisson":"fixed",rate,sent,nl,lost,late,lat[nl/2],lat[nl*9/10],lat[nl*99/100],lat[nl*999/1000],lat[nl-1]);}
  else printf("pace: no replies (sent %d)\n",sent);
  return 0;}
