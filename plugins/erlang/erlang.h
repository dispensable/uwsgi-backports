#include <ei.h>

#define MAX_UWSGI_ERLANG_PROCESSES 64

#define LONG_ARGS_ERLANG                17012
#define LONG_ARGS_ERLANG_COOKIE         17013



struct uwsgi_erlang_process {
	
	char name[0xff];
	void (*plugin)(void *, ei_x_buff *);
	void *func;
};

struct uwsgi_erlang {

        ei_cnode cnode;
        char *name;
        char *cookie;

        int fd;

	void *lock;

	struct uwsgi_erlang_process uep[MAX_UWSGI_ERLANG_PROCESSES];
	int uep_cnt;
};

