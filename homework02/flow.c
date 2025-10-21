#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>

#define MAX_NODES   128
#define MAX_PIPES   256
#define MAX_FILES   128
#define MAX_CONCATS 64
#define MAX_PARTS   64
#define MAX_NAME    128
#define MAX_LINE    1024

typedef struct { char name[MAX_NAME]; char cmdline[MAX_LINE]; int has_cmd; } Node;
typedef struct { char name[MAX_NAME]; char from[MAX_NAME]; char to[MAX_NAME]; int has_from, has_to; } FlowPipe;
typedef struct { char name[MAX_NAME]; int parts; char part[MAX_PARTS][MAX_NAME]; } Concat;
typedef struct { char name[MAX_NAME]; char path[MAX_LINE]; } FileComp;
typedef struct { char name[MAX_NAME]; char from_node[MAX_NAME]; int has_from; } StderrComp;

static Node g_nodes[MAX_NODES]; static int g_nodes_n = 0;
static FlowPipe g_pipes[MAX_PIPES]; static int g_pipes_n = 0;
static Concat g_concats[MAX_CONCATS]; static int g_concats_n = 0;
static FileComp g_files[MAX_FILES]; static int g_files_n = 0;
static StderrComp g_stderrs[MAX_NODES]; static int g_stderrs_n = 0;

static void die(const char *m){ perror(m); exit(1); }
static void safe_close(int fd){ if(fd>=0){ while(close(fd)==-1 && errno==EINTR){} } }

static ssize_t full_write(int fd, const void *buf, size_t n){
    const char *p=(const char*)buf; size_t left=n;
    while(left){
        ssize_t w=write(fd,p,left);
        if(w<0){ if(errno==EINTR) continue; return -1; }
        left-=w; p+=w;
    }
    return (ssize_t)n;
}
static int copy_stream(int in_fd,int out_fd){
    char buf[1<<15];
    for(;;){
        ssize_t r=read(in_fd,buf,sizeof(buf));
        if(r==0) return 0;
        if(r<0){ if(errno==EINTR) continue; return -1; }
        if(full_write(out_fd,buf,(size_t)r)<0) return -1;
    }
}

static void rstrip(char *s){ size_t n=strlen(s); while(n&& (s[n-1]=='\n'||s[n-1]=='\r'||isspace((unsigned char)s[n-1]))) s[--n]='\0'; }
static void lstrip(char *s){ size_t i=0; while(s[i] && isspace((unsigned char)s[i])) i++; if(i) memmove(s,s+i,strlen(s+i)+1); }
static void trim(char *s){ lstrip(s); rstrip(s); }
static int startswith(const char *s,const char *p){ return strncmp(s,p,strlen(p))==0; }

static Node* find_node(const char *n){ for(int i=0;i<g_nodes_n;i++) if(strcmp(g_nodes[i].name,n)==0) return &g_nodes[i]; return NULL; }
static FlowPipe* find_pipe(const char *n){ for(int i=0;i<g_pipes_n;i++) if(strcmp(g_pipes[i].name,n)==0) return &g_pipes[i]; return NULL; }
static Concat* find_concat(const char *n){ for(int i=0;i<g_concats_n;i++) if(strcmp(g_concats[i].name,n)==0) return &g_concats[i]; return NULL; }
static FileComp* find_filecomp(const char *n){ for(int i=0;i<g_files_n;i++) if(strcmp(g_files[i].name,n)==0) return &g_files[i]; return NULL; }
static StderrComp* find_stderrcomp(const char *n){ for(int i=0;i<g_stderrs_n;i++) if(strcmp(g_stderrs[i].name,n)==0) return &g_stderrs[i]; return NULL; }

static pid_t spawn_shell_with_fds(const char *cmd,int in_fd,int out_fd,int err_to_out){
    pid_t pid=fork(); if(pid<0) die("fork");
    if(pid==0){
        if(in_fd!=STDIN_FILENO){ if(dup2(in_fd,STDIN_FILENO)<0) die("dup2 in"); safe_close(in_fd); }
        if(out_fd!=STDOUT_FILENO){ if(dup2(out_fd,STDOUT_FILENO)<0) die("dup2 out"); safe_close(out_fd); }
        if(err_to_out){ if(dup2(STDOUT_FILENO,STDERR_FILENO)<0) die("dup2 err"); }
        execl("/bin/sh","sh","-c",cmd,(char*)NULL);
        perror("execl"); _exit(127);
    }
    return pid;
}

static int run_component_to_fd(const char *comp_name,int out_fd); /* forward */

static int run_node_to_fd(const char *node_name,int out_fd){
    Node *n=find_node(node_name);
    if(!n || !n->has_cmd){ fprintf(stderr,"Unknown or incomplete node: %s\n",node_name); return -1; }
    int in_dup=dup(STDIN_FILENO); if(in_dup<0) die("dup stdin");
    pid_t pid=spawn_shell_with_fds(n->cmdline,in_dup,out_fd,0);
    int st; if(waitpid(pid,&st,0)<0) die("waitpid node");
    return (WIFEXITED(st) && WEXITSTATUS(st)==0)?0:-1;
}

static int run_stderr_to_fd(const char *stderr_name,int out_fd){
    StderrComp *sc=find_stderrcomp(stderr_name);
    if(!sc || !sc->has_from){ fprintf(stderr,"Unknown or incomplete stderr: %s\n",stderr_name); return -1; }
    Node *n=find_node(sc->from_node);
    if(!n || !n->has_cmd){ fprintf(stderr,"stderr from unknown node: %s\n",sc->from_node); return -1; }
    int in_dup=dup(STDIN_FILENO); if(in_dup<0) die("dup stdin");
    pid_t pid=spawn_shell_with_fds(n->cmdline,in_dup,out_fd,1);
    int st; if(waitpid(pid,&st,0)<0) die("waitpid stderr");
    return (WIFEXITED(st) && WEXITSTATUS(st)==0)?0:-1;
}

static int run_file_to_fd(const char *filecomp_name,int out_fd){
    FileComp *fc=find_filecomp(filecomp_name);
    if(!fc){ fprintf(stderr,"Unknown file component: %s\n",filecomp_name); return -1; }
    int infd=open(fc->path,O_RDONLY);
    if(infd<0){ perror(fc->path); return -1; }
    int rc=0; if(copy_stream(infd,out_fd)<0){ perror("copy file"); rc=-1; }
    safe_close(infd); return rc;
}

static int run_concat_to_fd(const char *concat_name,int out_fd){
    Concat *c=find_concat(concat_name);
    if(!c){ fprintf(stderr,"Unknown concatenate: %s\n",concat_name); return -1; }
    for(int i=0;i<c->parts;i++){
        int pfd[2]; if(pipe(pfd)<0) die("pipe concat");
        int rc=run_component_to_fd(c->part[i],pfd[1]);
        safe_close(pfd[1]);
        if(rc!=0){ safe_close(pfd[0]); return -1; }
        if(copy_stream(pfd[0],out_fd)<0){ perror("concat copy"); safe_close(pfd[0]); return -1; }
        safe_close(pfd[0]);
    }
    return 0;
}

static int open_sink_fd_for_to(const char *to_name, Node **dst_node){
    FileComp *f=find_filecomp(to_name);
    if(f){
        int fd=open(f->path,O_CREAT|O_WRONLY|O_TRUNC,0666);
        if(fd<0){ perror(f->path); return -1; }
        return fd;
    }
    *dst_node=find_node(to_name);
    if(!*dst_node){ fprintf(stderr,"Pipe to= unknown component: %s\n",to_name); return -1; }
    return -3;
}

static int run_pipe_to_fd(const char *pipe_name,int out_fd){
    FlowPipe *p=find_pipe(pipe_name);
    if(!p || !p->has_from || !p->has_to){ fprintf(stderr,"Unknown or incomplete pipe: %s\n",pipe_name); return -1; }

    Node *dst=NULL;
    int sinkfd=open_sink_fd_for_to(p->to,&dst);
    if(sinkfd==-1) return -1;

    int linkfd[2]; if(pipe(linkfd)<0) die("pipe link");

    pid_t src_pid=fork();
    if(src_pid<0) die("fork pipe-src");
    if(src_pid==0){
        safe_close(linkfd[0]);
        int rc=run_component_to_fd(p->from,linkfd[1]);
        safe_close(linkfd[1]);
        _exit(rc==0?0:1);
    }

    if(sinkfd==-3){
        pid_t dst_pid=fork();
        if(dst_pid<0) die("fork pipe-dst");
        if(dst_pid==0){
            if(dup2(linkfd[0],STDIN_FILENO)<0) die("dup2 link->stdin");
            if(out_fd!=STDOUT_FILENO){ if(dup2(out_fd,STDOUT_FILENO)<0) die("dup2 out_fd"); }
            safe_close(linkfd[0]); safe_close(linkfd[1]);
            if(out_fd!=STDOUT_FILENO) safe_close(out_fd);
            execl("/bin/sh","sh","-c",dst->cmdline,(char*)NULL);
            perror("execl dst"); _exit(127);
        }
        safe_close(linkfd[0]); safe_close(linkfd[1]);
        int s1=-1,s2=-1,rc=0;
        if(waitpid(src_pid,&s1,0)<0) die("wait src");
        if(waitpid(dst_pid,&s2,0)<0) die("wait dst");
        if(!(WIFEXITED(s1)&&WEXITSTATUS(s1)==0)) rc=-1;
        if(!(WIFEXITED(s2)&&WEXITSTATUS(s2)==0)) rc=-1;
        return rc;
    }else{
        safe_close(linkfd[1]);
        int rc=0; if(copy_stream(linkfd[0],sinkfd)<0){ perror("pipe->file copy"); rc=-1; }
        safe_close(linkfd[0]); safe_close(sinkfd);
        int s=-1; if(waitpid(src_pid,&s,0)<0) die("wait src file");
        if(!(WIFEXITED(s)&&WEXITSTATUS(s)==0)) rc=-1;
        return rc;
    }
}

static const char *call_stack[512]; static int call_sp=0;
static int on_stack(const char *name){ for(int i=0;i<call_sp;i++) if(strcmp(call_stack[i],name)==0) return 1; return 0; }

static int run_component_to_fd_inner(const char *comp_name,int out_fd){
    if(find_node(comp_name))       return run_node_to_fd(comp_name,out_fd);
    if(find_pipe(comp_name))       return run_pipe_to_fd(comp_name,out_fd);
    if(find_concat(comp_name))     return run_concat_to_fd(comp_name,out_fd);
    if(find_stderrcomp(comp_name)) return run_stderr_to_fd(comp_name,out_fd);
    if(find_filecomp(comp_name))   return run_file_to_fd(comp_name,out_fd);
    fprintf(stderr,"Unknown component referenced: %s\n",comp_name);
    return -1;
}
static int run_component_to_fd(const char *comp_name,int out_fd){
    if(on_stack(comp_name)){ fprintf(stderr,"Cyclic dependency detected at '%s'\n",comp_name); return -1; }
    call_stack[call_sp++]=comp_name;
    int rc=run_component_to_fd_inner(comp_name,out_fd);
    call_sp--; return rc;
}

static void parse_flow_file(const char *path){
    FILE *fp=fopen(path,"r"); if(!fp) die("open flowfile");
    Node *currNode=NULL; FlowPipe *currPipe=NULL; Concat *currConcat=NULL; FileComp *currFile=NULL; StderrComp *currStderr=NULL;
    char raw[MAX_LINE];
    while(fgets(raw,sizeof(raw),fp)){
        char line[MAX_LINE]; strncpy(line,raw,sizeof(line)-1); line[sizeof(line)-1]='\0';
        trim(line);
        if(line[0]=='\0') continue;
        if(line[0]=='#') continue;
        if(startswith(line,"//")) continue;
        if(startswith(line,"node=")){
            currNode=NULL; currPipe=NULL; currConcat=NULL; currFile=NULL; currStderr=NULL;
            if(g_nodes_n>=MAX_NODES){ fprintf(stderr,"Too many nodes\n"); exit(1); }
            Node *n=&g_nodes[g_nodes_n++]; memset(n,0,sizeof(*n));
            snprintf(n->name,sizeof(n->name),"%s",line+5);
            currNode=n; continue;
        }
        if(startswith(line,"pipe=")){
            currNode=NULL; currPipe=NULL; currConcat=NULL; currFile=NULL; currStderr=NULL;
            if(g_pipes_n>=MAX_PIPES){ fprintf(stderr,"Too many pipes\n"); exit(1); }
            FlowPipe *p=&g_pipes[g_pipes_n++]; memset(p,0,sizeof(*p));
            snprintf(p->name,sizeof(p->name),"%s",line+5);
            currPipe=p; continue;
        }
        if(startswith(line,"concatenate=")){
            currNode=NULL; currPipe=NULL; currConcat=NULL; currFile=NULL; currStderr=NULL;
            if(g_concats_n>=MAX_CONCATS){ fprintf(stderr,"Too many concatenates\n"); exit(1); }
            Concat *c=&g_concats[g_concats_n++]; memset(c,0,sizeof(*c));
            snprintf(c->name,sizeof(c->name),"%s",line+12);
            currConcat=c; continue;
        }
        if(startswith(line,"file=")){
            currNode=NULL; currPipe=NULL; currConcat=NULL; currFile=NULL; currStderr=NULL;
            if(g_files_n>=MAX_FILES){ fprintf(stderr,"Too many files\n"); exit(1); }
            FileComp *fc=&g_files[g_files_n++]; memset(fc,0,sizeof(*fc));
            snprintf(fc->name,sizeof(fc->name),"%s",line+5);
            currFile=fc; continue;
        }
        if(startswith(line,"stderr=")){
            currNode=NULL; currPipe=NULL; currConcat=NULL; currFile=NULL; currStderr=NULL;
            if(g_stderrs_n>=MAX_NODES){ fprintf(stderr,"Too many stderr comps\n"); exit(1); }
            StderrComp *sc=&g_stderrs[g_stderrs_n++]; memset(sc,0,sizeof(*sc));
            snprintf(sc->name,sizeof(sc->name),"%s",line+7);
            currStderr=sc; continue;
        }
        if(startswith(line,"command=")){
            if(!currNode){ fprintf(stderr,"command= must follow node=\n"); exit(1); }
            snprintf(currNode->cmdline,sizeof(currNode->cmdline),"%s",line+8);
            currNode->has_cmd=1; continue;
        }
        if(startswith(line,"from=")){
            if(currPipe){ snprintf(currPipe->from,sizeof(currPipe->from),"%s",line+5); currPipe->has_from=1; continue; }
            if(currStderr){ snprintf(currStderr->from_node,sizeof(currStderr->from_node),"%s",line+5); currStderr->has_from=1; continue; }
            fprintf(stderr,"from= without context\n"); exit(1);
        }
        if(startswith(line,"to=")){
            if(!currPipe){ fprintf(stderr,"to= must follow pipe=\n"); exit(1); }
            snprintf(currPipe->to,sizeof(currPipe->to),"%s",line+3);
            currPipe->has_to=1; continue;
        }
        if(startswith(line,"parts=")){
            if(!currConcat){ fprintf(stderr,"parts= must follow concatenate=\n"); exit(1); }
            currConcat->parts=atoi(line+6);
            if(currConcat->parts<0 || currConcat->parts>MAX_PARTS){ fprintf(stderr,"parts out of range\n"); exit(1); }
            continue;
        }
        if(startswith(line,"part_")){
            if(!currConcat){ fprintf(stderr,"part_* must follow concatenate=\n"); exit(1); }
            char *eq=strchr(line,'='); if(!eq){ fprintf(stderr,"Malformed part_ line\n"); exit(1); }
            *eq='\0';
            int idx=atoi(line+5);
            if(idx<0 || idx>=MAX_PARTS){ fprintf(stderr,"part index out of range\n"); exit(1); }
            snprintf(currConcat->part[idx],sizeof(currConcat->part[idx]),"%s",eq+1);
            continue;
        }
        if(startswith(line,"name=")){
            if(!currFile){ fprintf(stderr,"name= must follow file=\n"); exit(1); }
            snprintf(currFile->path,sizeof(currFile->path),"%s",line+5);
            continue;
        }
        fprintf(stderr,"Unrecognized line in flow: %s\n",line);
        exit(1);
    }
    fclose(fp);
}

int main(int argc,char **argv){
    if(argc!=3){ fprintf(stderr,"Usage: %s <flowfile> <final_component>\n",argv[0]); return 1; }
    parse_flow_file(argv[1]);
    for(int i=0;i<g_nodes_n;i++) if(!g_nodes[i].has_cmd){ fprintf(stderr,"Node '%s' missing command=\n",g_nodes[i].name); return 1; }
    for(int i=0;i<g_pipes_n;i++) if(!g_pipes[i].has_from || !g_pipes[i].has_to){ fprintf(stderr,"Pipe '%s' missing from=/to=\n",g_pipes[i].name); return 1; }
    for(int i=0;i<g_stderrs_n;i++){
        if(!g_stderrs[i].has_from){ fprintf(stderr,"stderr '%s' missing from=\n",g_stderrs[i].name); return 1; }
        if(!find_node(g_stderrs[i].from_node)){ fprintf(stderr,"stderr '%s' refers to unknown node '%s'\n",g_stderrs[i].name,g_stderrs[i].from_node); return 1; }
    }
    for(int i=0;i<g_concats_n;i++){
        for(int k=0;k<g_concats[i].parts;k++){
            if(g_concats[i].part[k][0]=='\0'){ fprintf(stderr,"concatenate '%s' missing part_%d\n",g_concats[i].name,k); return 1; }
        }
    }
    int rc=run_component_to_fd(argv[2],STDOUT_FILENO);
    return (rc==0)?0:1;
}
