#include "io.h"

// we are using C-style IO here, as C++ streams do not allow non-blocking IO
#include <cstdio>
#include <termios.h>
#include <fcntl.h>

struct termios saved_attr, pty_saved_attr;

void io_init(bool skip_pty) {
  fcntl(0, F_SETFL, O_NONBLOCK);
  struct termios nattr;
  tcgetattr(0, &saved_attr);
  tcgetattr(0, &nattr);
  
  nattr.c_lflag &= ~(ICANON|ECHO);
  nattr.c_cc[VMIN] = 1;
  nattr.c_cc[VTIME] = 0;
  
  tcsetattr(0, TCSAFLUSH, &nattr);
  
  pty_init(skip_pty);
}
void io_uninit() {
  tcsetattr(0, TCSANOW, &saved_attr);
  
  pty_uninit();
}

void dbg_print(const char* msg){
  printf("%s",msg);
  fflush(stdout);
}

void dbg_print(int64_t num, bool hex){
  if (hex) {
    printf("%lx",num);
  } else {
    printf("%ld",num);
  }
}

void dbg_endl(){
  printf("\n");
  fflush(stdout);
}

void dbgerr_print(const char* msg){
  fprintf(stderr,"%s",msg);
  //fflush(stderr);
}

void dbgerr_print(int64_t num, bool hex){
  if (hex) {
    fprintf(stderr,"%lx",num);
  } else {
    fprintf(stderr,"%ld",num);
  }
}

void dbgerr_endl(){
  fprintf(stderr,"\n");
  //fflush(stderr);
}

char dbg_getc() {
  return getchar();
}

#include <pty.h>
#include <unistd.h>
#include <linux/limits.h>

int pty_master, pty_slave;
FILE *pty_master_in, *pty_master_out;
char *pty_slave_name;

bool dbg_fallback = false;

void pty_init(bool skip) {
  pty_slave_name = new char[PATH_MAX];
  if (skip) {
    dbgerr_print("PTY init skipped, falling back to emulated IO through debug");
    dbgerr_endl();
    dbg_fallback = true;
  } else if (!openpty(&pty_master, &pty_slave, pty_slave_name, NULL, NULL) ) {
    fcntl(pty_master, F_SETFL, O_NONBLOCK);
    struct termios nattr;
    tcgetattr(pty_master, &saved_attr);
    tcgetattr(pty_master, &nattr);
    
    nattr.c_lflag &= ~(ICANON|ECHO);
    nattr.c_cc[VMIN] = 1;
    nattr.c_cc[VTIME] = 0;
    
    tcsetattr(pty_master, TCSAFLUSH, &nattr);
  
    dbg_print("PTY slave filename: ");
    dbg_print(pty_slave_name);
    dbg_endl();
    
    pty_master_in = fdopen(pty_master, "r");
    pty_master_out = fdopen(pty_master, "w");
  } else {
    dbgerr_print("Could not open PTY, falling back to emulated IO through debug");
    dbgerr_endl();
    dbg_fallback = true;
  }
}
void pty_uninit() {
  tcsetattr(pty_master, TCSANOW, &pty_saved_attr);
  delete pty_slave_name;
  close(pty_master);
  close(pty_slave);
}

void pty_print(const char* msg) {
  if (dbg_fallback) {
    dbg_print(msg);
    return;
  }
  fprintf(pty_master_out, "%s", msg);
  fflush(pty_master_out);
}
char pty_getc() {
  if (dbg_fallback) {
    return dbg_getc();
  }
  return getc(pty_master_in);
}
void pty_endl() {
  fprintf(pty_master_out, "\n");
  fflush(pty_master_out);
}
