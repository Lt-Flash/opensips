/* burst latency probe: B bursts of N OPTIONS from N distinct UDP sockets,
 * sent back-to-back; per-reply latency and per-burst completion time. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e6+t.tv_nsec/1e3;}
static int cmp(const void*a,const void*b){double x=*(double*)a,y=*(double*)b;return x<y?-1:x>y;}
int main(int argc,char**argv){
  if(argc<6){fprintf(stderr,"usage: burst <ip> <port> <N> <bursts> <gap_ms>\n");return 1;}
  const char*ip=argv[1];int port=atoi(argv[2]),N=atoi(argv[3]),B=atoi(argv[4]),gap=atoi(argv[5]);
  struct sockaddr_in dst={0};dst.sin_family=AF_INET;dst.sin_port=htons(port);inet_pton(AF_INET,ip,&dst.sin_addr);
  int*fd=calloc(N,sizeof(int));struct pollfd*pf=calloc(N,sizeof(*pf));
  char lip[64]="0.0.0.0";{int t=socket(AF_INET,SOCK_DGRAM,0);connect(t,(struct sockaddr*)&dst,sizeof dst);struct sockaddr_in me;socklen_t l=sizeof me;getsockname(t,(struct sockaddr*)&me,&l);inet_ntop(AF_INET,&me.sin_addr,lip,sizeof lip);close(t);}
  int*lport=calloc(N,sizeof(int));
  for(int i=0;i<N;i++){fd[i]=socket(AF_INET,SOCK_DGRAM,0);struct sockaddr_in me={0};me.sin_family=AF_INET;bind(fd[i],(struct sockaddr*)&me,sizeof me);socklen_t l=sizeof me;getsockname(fd[i],(struct sockaddr*)&me,&l);lport[i]=ntohs(me.sin_port);pf[i].fd=fd[i];pf[i].events=POLLIN;}
  double*lat=calloc((size_t)N*B,sizeof(double));int nl=0,lost=0;double*bt=calloc(B,sizeof(double));
  char msg[N][512];double*ts=calloc(N,sizeof(double));char buf[2048];
  for(int b=0;b<B;b++){
    for(int i=0;i<N;i++) snprintf(msg[i],sizeof msg[i],
      "OPTIONS sip:ping@%s:%d SIP/2.0\r\nVia: SIP/2.0/UDP %s:%d;rport;branch=z9hG4bKb%dn%d\r\nFrom: <sip:b@x>;tag=%d\r\nTo: <sip:ping@%s>\r\nCall-ID: burst%d-%d\r\nCSeq: 1 OPTIONS\r\nMax-Forwards: 70\r\nContent-Length: 0\r\n\r\n",ip,port,lip,lport[i],b,i,i,ip,b,i);
    int got=0;double t0=now();
    for(int i=0;i<N;i++){ts[i]=now();sendto(fd[i],msg[i],strlen(msg[i]),0,(struct sockaddr*)&dst,sizeof dst);}
    double deadline=t0+300000;double last=t0;
    while(got<N){double left=(deadline-now())/1000;if(left<=0)break;int r=poll(pf,N,(int)left+1);if(r<=0)break;
      for(int i=0;i<N&&r>0;i++) if(pf[i].revents&POLLIN){double t=now();recv(fd[i],buf,sizeof buf,0);lat[nl++]=t-ts[i];got++;r--;if(t>last)last=t;pf[i].revents=0;}}
    lost+=N-got;bt[b]=last-t0;
    usleep(gap*1000);
  }
  qsort(lat,nl,sizeof(double),cmp);qsort(bt,B,sizeof(double),cmp);
  printf("burst N=%d B=%d replies=%d lost=%d | reply latency us: p50 %.0f p90 %.0f p99 %.0f max %.0f | burst completion us: p50 %.0f p90 %.0f max %.0f\n",
    N,B,nl,lost,lat[nl/2],lat[nl*9/10],lat[nl*99/100],lat[nl-1],bt[B/2],bt[B*9/10],bt[B-1]);
  return 0;}
