#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* This is a small, fixed-action privileged verifier, NOT a command runner.
 * Only root-owned hashes are authoritative. No path/command is user-selected.
 * Install root:root mode 4755 only after verifying the build and enrollment.
 */
#define STORE "/var/lib/alternix-auth"
#define ITERATIONS 240000
#define SECRET_MAX 1024
#define GUI "/usr/local/bin/osm-lock"
static uid_t owner;
static void wipe(void *p,size_t n){ OPENSSL_cleanse(p,n); }
static int root_dir(void){
 struct stat st;
 if (lstat(STORE,&st)<0 || !S_ISDIR(st.st_mode) || st.st_uid!=0 || (st.st_mode & 077)!=0) return -1;
 return 0;
}
static int ufile(char *out,size_t len,const char *kind,uid_t uid){
 int n=snprintf(out,len,"%s/%s-%lu",STORE,kind,(unsigned long)uid);
 return n>0 && (size_t)n<len ? 0:-1;
}
static int read_secret(int fd,char *buf,size_t cap,int timeout){
 size_t n=0; int newline=0;
 while(n<cap-1){
  struct pollfd f={.fd=fd,.events=POLLIN|POLLHUP};
  int r=poll(&f,1,timeout);
  if(r<=0)return -1;
  char c;ssize_t z=read(fd,&c,1);
  if(z<=0)break;
  if(c=='\n'){newline=1;break;}
  if(c=='\r'||(unsigned char)c<32 || (unsigned char)c>126)return -1;
  buf[n++]=c;
 }
 buf[n]=0;
 return newline && n>1 ? 0:-1;
}
static int hexdecode(const char *hex,unsigned char *out,size_t len){
 if(strlen(hex)!=len*2)return -1;
 for(size_t i=0;i<len;i++){
  int a=hex[2*i],b=hex[2*i+1];
  a=a>='0'&&a<='9'?a-'0':a>='a'&&a<='f'?a-'a'+10:-1;
  b=b>='0'&&b<='9'?b-'0':b>='a'&&b<='f'?b-'a'+10:-1;
  if(a<0||b<0)return -1;
  out[i]=(unsigned char)(a*16+b);
 }
 return 0;
}
static void hexencode(const unsigned char *a,size_t n,char *out){
 static const char *h="0123456789abcdef";
 for(size_t i=0;i<n;i++){out[i*2]=h[a[i]>>4];out[i*2+1]=h[a[i]&15];}
 out[n*2]=0;
}
static int validate_payload(char *p,char *pin,int *enh){
 /* Pattern: 4/5 comma-separated lowercase SHA-256 shape identifiers.
  * PIN: 64 hex characters (SHA-256 output); root KDF protects storage.
  */
 int count=1;
 if(strlen(pin)!=64) return -1;
 for(char *s=pin;*s;s++)if(!((*s>='a'&&*s<='f')||(*s>='0'&&*s<='9')))return -1;
 size_t n=strlen(p);if(n<64*4+3||n>64*5+4)return -1;
 int t=0;
 for(size_t i=0;i<n;i++){
  char c=p[i];
  if(c==',') {if(t!=64)return -1;count++;t=0;}
  else{if(!((c>='a'&&c<='f')||(c>='0'&&c<='9')))return -1;t++;}
 }
 if(t!=64 || count!=(*enh?5:4))return -1;
 return 0;
}
static int derive(const unsigned char *salt,const char *prefix,const char *in,unsigned char out[32]){
 char v[SECRET_MAX+8];
 if(snprintf(v,sizeof(v),"%s%s",prefix,in)>=(int)sizeof(v))return -1;
 int ok=PKCS5_PBKDF2_HMAC(v,-1,salt,16,ITERATIONS,EVP_sha256(),32,out);
 wipe(v,sizeof(v));return ok==1?0:-1;
}
static int create_credential(uid_t uid,const char *p,const char *pin,int enh){
 char path[256],tmp[256],ps[65],is[65],salt_hex[33];
 unsigned char salt[16],pd[32],id[32];
 if(root_dir()!=0 || ufile(path,sizeof(path),"credential",uid)!=0)return -1;
 if(access(path,F_OK)==0)return -1;
 if(RAND_bytes(salt,sizeof(salt))!=1 || derive(salt,"P:",p,pd)!=0 || derive(salt,"I:",pin,id)!=0)return -1;
 hexencode(salt,16,salt_hex);hexencode(pd,32,ps);hexencode(id,32,is);
 if(snprintf(tmp,sizeof(tmp),"%s/tmp-%lu-%ld",STORE,(unsigned long)uid,(long)getpid())>=(int)sizeof(tmp))return -1;
 int fd=open(tmp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
 if(fd<0)return -1;
 char body[350];int n=snprintf(body,sizeof(body),"v=1\nenhanced=%d\nsalt=%s\npattern=%s\npin=%s\n",enh,salt_hex,ps,is);
 int ok=n>0&&n<(int)sizeof(body)&&write(fd,body,n)==n&&fsync(fd)==0;
 close(fd);
 if(!ok || link(tmp,path)!=0){unlink(tmp);return -1;}
 unlink(tmp);wipe(body,sizeof(body));wipe(pd,sizeof(pd));wipe(id,sizeof(id));return 0;
}
static int load_credential(uid_t uid,int *enh,unsigned char salt[16],unsigned char pd[32],unsigned char id[32]){
 char path[256],buf[512]; struct stat st;
 if(root_dir()!=0 || ufile(path,sizeof(path),"credential",uid)!=0)return -1;
 int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
 if(fd<0)return -1;
 if(fstat(fd,&st)!=0 || !S_ISREG(st.st_mode)||st.st_uid!=0||(st.st_mode & 077)!=0){close(fd);return -1;}
 ssize_t n=read(fd,buf,sizeof(buf)-1);close(fd);
 if(n<0||n>=(ssize_t)sizeof(buf)-1)return -1;
 buf[n]=0;char *v[5]={0};
 for(char *s=strtok(buf,"\n");s;s=strtok(NULL,"\n")){
  if(!strncmp(s,"v=",2))v[0]=s+2;
  else if(!strncmp(s,"enhanced=",9))v[1]=s+9;
  else if(!strncmp(s,"salt=",5))v[2]=s+5;
  else if(!strncmp(s,"pattern=",8))v[3]=s+8;
  else if(!strncmp(s,"pin=",4))v[4]=s+4;
 }
 if(!v[0]||strcmp(v[0],"1")||!v[1]||(strcmp(v[1],"1")&&strcmp(v[1],"0"))||!v[2]||!v[3]||!v[4]||hexdecode(v[2],salt,16)||hexdecode(v[3],pd,32)||hexdecode(v[4],id,32))return -1;
 *enh=(*v[1]=='1');wipe(buf,sizeof(buf));return 0;
}
static int limited_check(uid_t uid,const char *kind,const char *value){
 char path[256]; int enhanced=0,ret=-1;unsigned char salt[16],pd[32],id[32],calc[32];
 if(load_credential(uid,&enhanced,salt,pd,id)!=0)return -1;
 if(ufile(path,sizeof(path),"attempts",uid)!=0)return -1;
 int fd=open(path,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
 if(fd<0)return -1;
 struct stat st;
 if(fstat(fd,&st)!=0||!S_ISREG(st.st_mode)||st.st_uid!=0||(st.st_mode&077)!=0||flock(fd,LOCK_EX)!=0){close(fd);return -1;}
 int failures=0;long until=0;char b[128]={0};ssize_t z=read(fd,b,sizeof(b)-1);
 if(z>0)sscanf(b,"%d %ld",&failures,&until);
 time_t now=time(NULL);
 if(until>now)goto done;
 if(until && until<=now){failures=0;until=0;}
 if((!strcmp(kind,"P:")||!strcmp(kind,"I:")) && derive(salt,kind,value,calc)==0){
  const unsigned char *expect=!strcmp(kind,"P:")?pd:id;
  if(CRYPTO_memcmp(calc,expect,32)==0){ret=0;failures=0;until=0;}
 }
 if(ret!=0){failures++;if(failures>=5){failures=0;until=(long)now+90;}}
 if(lseek(fd,0,SEEK_SET)<0||ftruncate(fd,0)!=0)goto done;
 int n=snprintf(b,sizeof(b),"%d %ld\n",failures,until);
 if(write(fd,b,n)!=n||fsync(fd)!=0)ret=-1;
 done: flock(fd,LOCK_UN);close(fd);wipe(calc,sizeof(calc));wipe(pd,sizeof(pd));wipe(id,sizeof(id));return ret;
}
static int get_payload(char *line,char **kind,char **data){
 if(!strncmp(line,"P:",2)){*kind="P:";*data=line+2;return 0;}
 if(!strncmp(line,"I:",2)){*kind="I:";*data=line+2;return 0;}
 return -1;
}
static int record_from_stdin(uid_t uid){
 char p[SECRET_MAX]={0},pin[128]={0},en[16]={0};
 if(read_secret(STDIN_FILENO,p,sizeof(p),15000)!=0||read_secret(STDIN_FILENO,pin,sizeof(pin),15000)!=0||read_secret(STDIN_FILENO,en,sizeof(en),15000)!=0)return 1;
 if(strncmp(p,"P:",2)||strncmp(pin,"I:",2)||strncmp(en,"E:",2))return 1;
 int enhanced=(en[2]=='1'&&en[3]==0)?1:(en[2]=='0'&&en[3]==0)?0:-1;
 if(enhanced<0 || validate_payload(p+2,pin+2,&enhanced)!=0)return 1;
 int r=create_credential(uid,p+2,pin+2,enhanced);
 wipe(p,sizeof(p));wipe(pin,sizeof(pin));return r==0?0:1;
}
static int launch_prompt(uid_t uid,char *line,size_t limit){
 int p[2];if(pipe2(p,O_CLOEXEC)!=0)return -1;
 pid_t pid=fork();if(pid<0){close(p[0]);close(p[1]);return -1;}
 if(pid==0){
  close(p[0]);
  if(p[1]!=3){if(dup2(p[1],3)!=3)_exit(125);close(p[1]);}
  else if(fcntl(3,F_SETFD,0)!=0)_exit(125);
  /* Unprivileged Qt child only. Never execute the GUI as root. */
  struct passwd *pw=getpwuid(uid);
  if(!pw||setgroups(0,NULL)!=0||setgid(pw->pw_gid)!=0||setuid(uid)!=0)_exit(125);
  const char *display0=getenv("DISPLAY"),*xa0=getenv("XAUTHORITY"),*runtime0=getenv("XDG_RUNTIME_DIR");
  const char *dbus0=getenv("DBUS_SESSION_BUS_ADDRESS");
  char *display=display0?strdup(display0):NULL,*xa=xa0?strdup(xa0):NULL;
  char *runtime=runtime0?strdup(runtime0):NULL,*dbus=dbus0?strdup(dbus0):NULL;
  char *home=strdup(pw->pw_dir),*name=strdup(pw->pw_name);
  if(!home||!name)_exit(125);
  clearenv();setenv("PATH","/usr/bin:/bin",1);setenv("HOME",home,1);setenv("USER",name,1);
  if(display&&strlen(display)<256)setenv("DISPLAY",display,1);
  if(xa&&strlen(xa)<4096)setenv("XAUTHORITY",xa,1);
  if(runtime&&strlen(runtime)<4096)setenv("XDG_RUNTIME_DIR",runtime,1);
  if(dbus&&strlen(dbus)<4096)setenv("DBUS_SESSION_BUS_ADDRESS",dbus,1);
  char *const a[]={ (char*)GUI,(char*)"--pam-prompt",NULL};execv(GUI,a);_exit(126);
 }
 close(p[1]);int r=read_secret(p[0],line,limit,120000);close(p[0]);
 if(r!=0)kill(pid,SIGTERM);
 int status=0;waitpid(pid,&status,0);
 return r==0&&WIFEXITED(status)&&WEXITSTATUS(status)==0?0:-1;
}
int main(int argc,char **argv){
 owner=getuid();
 if(geteuid()!=0||argc<2)return 1;
 if(owner==0 && argc==2 && !strcmp(argv[1],"--pam")){
  /* Some sudo builds temporarily use ruid=root during PAM setup.
   * PAM_RUSER is populated by pam_exec from the PAM handle, not by us.
   * This fallback is only reachable by an already-root caller. */
  const char *requester=getenv("PAM_RUSER");
  if(!requester || !*requester)return 1;
  struct passwd *p=getpwnam(requester);
  if(!p || p->pw_uid<100)return 1;
  owner=p->pw_uid;
 }
 if(owner==0 && strcmp(argv[1],"--import"))return 1;
 if(!strcmp(argv[1],"--import")){
  if(owner!=0||argc!=3)return 1;
  char *end=NULL;unsigned long x=strtoul(argv[2],&end,10);
  if(!end||*end||x<100||x>UINT32_MAX)return 1;
  return record_from_stdin((uid_t)x);
 }
 if(argc!=2)return 1;
 if(!strcmp(argv[1],"--status")){
  int e;unsigned char a[16],b[32],c[32];
  if(load_credential(owner,&e,a,b,c)==0){printf("READY:%d\n",e);return 0;}
  puts("NOT_ENROLLED");return 2;
 }
 /* Only an already-privileged installer may provision new credentials.
  * An unprivileged user process must never self-enrol then sudo as root. */
 if(!strcmp(argv[1],"--verify")){
  char line[SECRET_MAX];char *k,*v;
  if(read_secret(STDIN_FILENO,line,sizeof(line),15000)!=0||get_payload(line,&k,&v)!=0)return 1;
  int r=limited_check(owner,k,v);wipe(line,sizeof(line));return r==0?0:1;
 }
 if(!strcmp(argv[1],"--pam")){
  /* PAM_EXEC with seteuid runs us as euid=root, ruid=login user.
   * GUI returns a candidate, never an approval decision. */
  int e;unsigned char a[16],b[32],c[32];
  if(load_credential(owner,&e,a,b,c)!=0)return 1;
  char line[SECRET_MAX];char *k,*v;
  if(launch_prompt(owner,line,sizeof(line))!=0||get_payload(line,&k,&v)!=0)return 1;
  int r=limited_check(owner,k,v);wipe(line,sizeof(line));return r==0?0:1;
 }
 return 1;
}
