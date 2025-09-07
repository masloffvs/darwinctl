#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <libgen.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>

#ifndef UF_IMMUTABLE
#define UF_IMMUTABLE 0x00000002
#endif


// ---------- Paths ----------
static void expand_home(const char *in, char *out, size_t outsz) {
	if (!in) { *out = 0; return; }
	if (in[0] == '~') {
		const char *home = getenv("HOME");
		if (!home) home = "";
		snprintf(out, outsz, "%s%s", home, in+1);
	} else {
		snprintf(out, outsz, "%s", in);
	}
}

static void default_units_dir(char *out, size_t n) {
	expand_home("~/DarwinUnits", out, n);
}

static void log_path(char *out, size_t n) {
	expand_home("~/Library/Logs/darwinctl.log", out, n);
}

static void run_dir(char *out, size_t n) {
	expand_home("~/Library/Application Support/darwinctl/run", out, n);
}
static void state_path(char *out, size_t n) {
	expand_home("~/Library/Application Support/darwinctl/state.index", out, n);
}

// ---------- Logging ----------
static void ensure_parent_dirs(const char *path) {
	char tmp[PATH_MAX]; snprintf(tmp, sizeof(tmp), "%s", path);
	char *dir = dirname(tmp);
	char cmd[PATH_MAX+32];
	snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", dir);
	(void)system(cmd);
}
static void now_str(char *buf, size_t n) {
	time_t t=time(NULL); struct tm tm; localtime_r(&t,&tm);
	strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm);
}
static void logf_named(const char *name, const char *fmt, ...) {
	char lp[PATH_MAX]; log_path(lp, sizeof(lp));
	ensure_parent_dirs(lp);
	FILE *f = fopen(lp, "a");
	if (!f) return;
	char ts[32]; now_str(ts, sizeof(ts));

	va_list ap; va_start(ap, fmt);
	fprintf(f, "%s  %s: ", ts, name?name:"darwinctl");
	vfprintf(f, fmt, ap);
	fprintf(f, "\n");
	va_end(ap);
	fclose(f);
}
static void info(const char *fmt, ...) {
	va_list ap; va_start(ap, fmt);
	char ts[32]; now_str(ts, sizeof(ts));
	printf("%s  darwinctl: ", ts);
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
}

// ---------- Tiny TOML-ish parser (limited) ----------
typedef struct {
	char *name;
	char *exec;
	char *workdir;
	int autostart;
	char **after;
	int after_count;
} unit_t;

static char* sdup(const char *s){ if(!s) return NULL; size_t n=strlen(s)+1; char *p=malloc(n); if(p) memcpy(p,s,n); return p; }

static char *trim(char *s){
	if(!s) return s;
	while(isspace((unsigned char)*s)) s++;
	char *e=s+strlen(s);
	while(e>s && isspace((unsigned char)e[-1])) --e;
	*e=0;
	return s;
}
static int parse_string(const char *in, char *out, size_t n){
	// expects quoted "...."
	const char *p=in;
	while(isspace((unsigned char)*p)) p++;
	if(*p!='"') return -1;
	p++;
	size_t k=0;
	while(*p && *p!='"'){
		if(k+1<n){ out[k++]=*p; }
		p++;
	}
	if(*p!='"') return -1;
	out[k]=0;
	return 0;
}
static int parse_array_of_strings(const char *in, char ***out, int *count){
	*out=NULL; *count=0;
	const char *p=in;
	while(isspace((unsigned char)*p)) p++;
	if(*p!='[') return -1;
	p++;

	int cap=4, cnt=0;
	char **arr = malloc(sizeof(char*)*cap);
	if(!arr) return -1;

	for(;;){
		while(isspace((unsigned char)*p)) p++;
		if(*p==']'){ p++; break; }  
		if(*p==','){ p++; continue; }

		char buf[1024];
		if(parse_string(p, buf, sizeof(buf))!=0){
			free(arr);
			return -1;
		}

		const char *q = strchr(p, '"');    
		if(!q){ free(arr); return -1; }
		q = strchr(q+1, '"');              
		if(!q){ free(arr); return -1; }
		p = q + 1;

		if(cnt==cap){
			cap*=2;
			char **na=realloc(arr,sizeof(char*)*cap);
			if(!na){ free(arr); return -1; }
			arr=na;
		}
		arr[cnt++] = sdup(buf);

		while(isspace((unsigned char)*p)) p++;
		if(*p==','){ p++; }
	}
	*out=arr; *count=cnt;
	return 0;
}

static int load_unit_from_file(const char *path, unit_t *u) {
	memset(u,0,sizeof(*u));
	FILE *f=fopen(path,"r"); if(!f) return -1;
	char line[2048];
	while(fgets(line,sizeof(line),f)){
		char *s=trim(line);
		if(*s=='#' || *s==0) continue;
		char *eq=strchr(s,'=');
		if(!eq) continue;
		*eq=0;
		char *key=trim(s);
		char *val=trim(eq+1);

		if(strcmp(key,"name")==0){
			char buf[1024]; if(parse_string(val,buf,sizeof(buf))==0) u->name=sdup(buf);
		} else if(strcmp(key,"exec")==0){
			char buf[2048]; if(parse_string(val,buf,sizeof(buf))==0) u->exec=sdup(buf);
		} else if(strcmp(key,"workdir")==0){
			char buf[2048]; if(parse_string(val,buf,sizeof(buf))==0) u->workdir=sdup(buf);
		} else if(strcmp(key,"autostart")==0){
			if(strncasecmp(val,"true",4)==0) u->autostart=1;
			else u->autostart=0;
		} else if(strcmp(key,"after")==0){
			(void)parse_array_of_strings(val, &u->after, &u->after_count);
		}
	}
	fclose(f);
	if(!u->name || !u->exec){
		logf_named("darwinctl","%s: missing required keys", path);
		return -2;
	}
	return 0;
}

static void free_unit(unit_t *u){
	if(!u) return;
	free(u->name); free(u->exec); free(u->workdir);
	for(int i=0;i<u->after_count;i++) free(u->after[i]);
	free(u->after);
	memset(u,0,sizeof(*u));
}

// ---------- Units loading & index ----------
typedef struct {
	unit_t *items;
	int count;
} unit_list_t;

static int ends_with(const char *s, const char *suf){
	size_t n=strlen(s), m=strlen(suf);
	return n>=m && strcmp(s+n-m,suf)==0;
}

// ---------- rootinit ----------
static void ensure_rootinit_file(void){
	char dirp[PATH_MAX]; default_units_dir(dirp, sizeof(dirp));
	char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/%s", dirp, "rootinit.toml");

	struct stat st;
	if (stat(path, &st) == 0) return; // уже есть

	ensure_parent_dirs(path);
	FILE *f = fopen(path, "w");
	if (!f) { logf_named("darwinctl", "cannot create %s: %s", path, strerror(errno)); return; }
	// «пустой» якорь: редактируй чем хочешь
	fprintf(f,
		"name = \"rootinit\"\n"
		"exec = \"/usr/bin/true\"\n"
		"workdir = \"\"\n"
		"autostart = true\n"
		"after = []\n"
	);
	fclose(f);
	logf_named("darwinctl","created default rootinit at %s", path);
}


static int load_all_units(unit_list_t *L){
	memset(L,0,sizeof(*L));
	ensure_rootinit_file();
	char dirp[PATH_MAX]; default_units_dir(dirp, sizeof(dirp));
	DIR *d=opendir(dirp);
	if(!d){ logf_named("darwinctl","units dir not found: %s", dirp); return -1; }
	int cap=16, cnt=0;
	unit_t *arr=calloc(cap,sizeof(unit_t));
	struct dirent *de;
	while((de=readdir(d))){
		if(de->d_type==DT_REG && ends_with(de->d_name,".toml")){
			if(cnt==cap){ cap*=2; unit_t *na=realloc(arr,sizeof(unit_t)*cap); if(!na){closedir(d); free(arr); return -1;} arr=na; }
			char path[PATH_MAX]; snprintf(path,sizeof(path), "%s/%s", dirp, de->d_name);
			if(load_unit_from_file(path, &arr[cnt])==0){
				cnt++;
			}
		}
	}
	closedir(d);
	L->items=arr; L->count=cnt;
	return 0;
}

static int find_unit(unit_list_t *L, const char *name){
	for(int i=0;i<L->count;i++) if(strcmp(L->items[i].name, name)==0) return i;
	return -1;
}

// ---------- Dependency sort (Kahn) ----------
typedef struct { int *v; int n; } intvec_t;
static void intvec_push(intvec_t *iv, int x){ iv->v=realloc(iv->v,sizeof(int)*(iv->n+1)); iv->v[iv->n++]=x; }

static int * topo_order(unit_list_t *L, int *outN){
	int n=L->count;
	int *indeg=calloc(n,sizeof(int));
	int **adj = calloc(n,sizeof(int*));
	int *adjn = calloc(n,sizeof(int));
	// Build graph: edge j->i if unit i has after ... j
	for(int i=0;i<n;i++){
		unit_t *u=&L->items[i];
		for(int k=0;k<u->after_count;k++){
			int j=find_unit(L, u->after[k]);
			if(j<0){ logf_named(u->name,"unknown dependency '%s'", u->after[k]); }
			else {
				adj[j]=realloc(adj[j], sizeof(int)*(adjn[j]+1));
				adj[j][adjn[j]++]=i;
				indeg[i]++;
			}
		}
	}
	intvec_t Q={0};
	for(int i=0;i<n;i++) if(indeg[i]==0) intvec_push(&Q,i);
	int *order=malloc(sizeof(int)*n); int m=0;
	while(Q.n>0){
		int v=Q.v[--Q.n];
		order[m++]=v;
		for(int t=0;t<adjn[v];t++){
			int w=adj[v][t];
			if(--indeg[w]==0) intvec_push(&Q,w);
		}
	}
	free(Q.v); for(int i=0;i<n;i++) free(adj[i]); free(adj); free(adjn); free(indeg);
	if(m!=n){
		logf_named("darwinctl","dependency cycle detected");
		free(order); *outN=0; return NULL;
	}
	*outN=n; return order;
}

// ---------- start/stop ----------
static void pidfile_path(const char *name, char *out, size_t n){
	char rd[PATH_MAX]; run_dir(rd,sizeof(rd)); ensure_parent_dirs(rd);
	snprintf(out,n,"%s/%s.pid", rd, name);
}
static pid_t read_pidfile(const char *name){
	char p[PATH_MAX]; pidfile_path(name,p,sizeof(p));
	FILE *f=fopen(p,"r"); if(!f) return 0;
	long v=0; if(fscanf(f,"%ld",&v)!=1){ fclose(f); return 0; }
	fclose(f);
	return (pid_t)v;
}
static int write_pidfile(const char *name, pid_t pid){
	char p[PATH_MAX]; pidfile_path(name,p,sizeof(p));
	ensure_parent_dirs(p);
	FILE *f=fopen(p,"w"); if(!f) return -1;
	fprintf(f,"%ld",(long)pid);
	fclose(f);
	return 0;
}
static int remove_pidfile(const char *name){
	char p[PATH_MAX]; pidfile_path(name,p,sizeof(p));
	unlink(p);
	return 0;
}

/** Start a single unit:
 *   - Forks once; child optionally chdir()s to workdir and execs via /bin/sh -c <exec>.
 *   - Parent writes pidfile and logs.
 *  @return 0 on parent side success, <0 on fork failure.
 *  @security Uses shell interpretation (/bin/sh -c). Ensure 'exec' is trusted.
 *  @todo Consider posix_spawn with argv splitting and no shell.
 */
static int start_unit(const unit_t *u){
	pid_t pid=fork();
	if(pid<0){ logf_named(u->name,"fork failed: %s", strerror(errno)); return -1; }
	if(pid==0){
		if(u->workdir && *u->workdir) chdir(u->workdir);
		execl("/bin/sh","sh","-c",u->exec,(char*)NULL);
		_exit(127);
	}
	write_pidfile(u->name,pid);
	logf_named(u->name,"started pid=%ld cmd=%s",(long)pid,u->exec);
	return 0;
}

/** Stop a single unit by reading its pidfile and signaling the process.
 *  SIGTERM, wait up to ~5s, then SIGKILL if still alive. Removes pidfile.
 */
static int stop_unit(const unit_t *u){
	pid_t pid=read_pidfile(u->name);
	if(pid<=0){ logf_named(u->name,"no pidfile"); return -1; }
	if(kill(pid, SIGTERM)==0){
		for(int i=0;i<50;i++){ // wait up to ~5s
			int st; pid_t r=waitpid(pid,&st,WNOHANG);
			if(r==pid) break;
			usleep(100000);
		}
		if(kill(pid,0)==0){
			kill(pid,SIGKILL);
		}
	}
	remove_pidfile(u->name);
	logf_named(u->name,"stopped");
	return 0;
}

/** Edit a unit file safely:
 *   - Creates a stub if missing.
 *   - For existing files: temporarily clears macOS UF_IMMUTABLE, runs $EDITOR, restores flag.
 *  @return 0 always (logs failures).
 *  @platform macOS: uses chflags(2) UF_IMMUTABLE.
 */
 static int edit_unit(const char *unit_name){
	char dirp[PATH_MAX]; default_units_dir(dirp,sizeof(dirp));
	char path[PATH_MAX]; snprintf(path,sizeof(path), "%s/%s.toml", dirp, unit_name);

	struct stat st;
	int existed = (stat(path,&st)==0);
	if(!existed){
		ensure_parent_dirs(path);
		FILE *f=fopen(path,"w");
		if(f){ fprintf(f,"name = \"%s\"\nexec = \"\"\nautostart = false\nafter = []\n", unit_name); fclose(f);}
	} else {
		// check immutable flag
		unsigned int had_uchg = (st.st_flags & UF_IMMUTABLE) ? 1 : 0;
		if(had_uchg){
			if(chflags(path, st.st_flags & ~UF_IMMUTABLE)!=0){
				info("Warning: cannot clear uchg on %s: %s", path, strerror(errno));
			}
		}
		// open editor
		const char *ed = getenv("EDITOR"); if(!ed||!*ed) ed="/usr/bin/nano";
		pid_t pid=fork();
		if(pid==0){ execlp(ed, ed, path, (char*)NULL); _exit(127); }
		int st2; waitpid(pid,&st2,0);
		// restore uchg if was set
		if(had_uchg){
			struct stat st3; if(stat(path,&st3)==0){
				chflags(path, st3.st_flags | UF_IMMUTABLE);
			}
		}
		return 0;
	}
	const char *ed = getenv("EDITOR"); if(!ed||!*ed) ed="/usr/bin/nano";
	pid_t pid=fork();
	if(pid==0){ execlp(ed, ed, path, (char*)NULL); _exit(127); }
	int st2; waitpid(pid,&st2,0);
	return 0;
}

// ---------- refresh ----------
/** Write a simple state index listing all units; used for diagnostics/debug. */
// But why? I want..
// Say less
static void write_state_index(unit_list_t *L){
	char sp[PATH_MAX]; state_path(sp,sizeof(sp));
	ensure_parent_dirs(sp);
	FILE *f=fopen(sp,"w"); if(!f) return;
	fprintf(f,"# darwinctl index\nunits=%d\n", L->count);
	for(int i=0;i<L->count;i++){
		fprintf(f,"- %s\n", L->items[i].name);
	}
	fclose(f);
}


static int cmd_stop(const char *name){
	unit_list_t L; if(load_all_units(&L)!=0){ info("No units"); return 1; }
	int i=find_unit(&L,name);
	if(i<0){ info("Unit not found: %s", name); goto out; }
	stop_unit(&L.items[i]);
out:
	for(int k=0;k<L.count;k++) free_unit(&L.items[k]); free(L.items);
	return 0;
}

static int cmd_refresh(){
	unit_list_t L; if(load_all_units(&L)!=0){ info("No units"); return 1; }
	int N=0; int *ord=topo_order(&L,&N);
	if(!ord){ info("Dependency cycle detected"); }
	write_state_index(&L);
	info("Refreshed %d units (indexed only).", L.count);
	if(ord) free(ord);
	for(int i=0;i<L.count;i++) free_unit(&L.items[i]); free(L.items);
	return 0;
}
static int cmd_edit(const char *name){
	return edit_unit(name);
}

// ---------- dependency graph helpers ----------
typedef struct {
	int **adj;
	int *adjn;
} graph_t;

static void graph_init(graph_t *g, int n){
	g->adj = calloc(n, sizeof(int*));
	g->adjn = calloc(n, sizeof(int));
}
static void graph_free(graph_t *g, int n){
	for(int i=0;i<n;i++) free(g->adj[i]);
	free(g->adj); free(g->adjn);
}

static void build_graph(unit_list_t *L, graph_t *g){
	int n=L->count;
	graph_init(g, n);
	for(int i=0;i<n;i++){
		unit_t *u=&L->items[i];
		for(int k=0;k<u->after_count;k++){
			int j = find_unit(L, u->after[k]);
			if(j<0){
				logf_named(u->name,"unknown dependency '%s'", u->after[k]);
				continue;
			}
			g->adj[j] = realloc(g->adj[j], sizeof(int)*(g->adjn[j]+1));
			g->adj[j][ g->adjn[j]++ ] = i;
		}
	}
}

static unit_list_t *g_sort_L = NULL;
static int cmp_child_idx(const void *a, const void *b){
	int ia = *(const int*)a, ib = *(const int*)b;
	return strcmp(g_sort_L->items[ia].name, g_sort_L->items[ib].name);
}

static void print_indent(int depth){
	for(int i=0;i<depth;i++) printf("  ");
}

static void print_map_rec(unit_list_t *L, graph_t *G, int v, int depth, char *vis){
	// vis: 0 = new, 1 = visiting, 2 = done
	if(vis[v]==1){
		print_indent(depth);
		printf("↳ %s (cycle)\n", L->items[v].name);
		return;
	}
	if(vis[v]==2) {
		print_indent(depth);
		printf("↳ %s (seen)\n", L->items[v].name);
		return;
	}
	vis[v]=1;

	print_indent(depth);
	printf("↳ %s\n", L->items[v].name);

	// sorting children by name
	// has never been so close to failure Stirlec
	if(G->adjn[v]>0){
		int n=G->adjn[v];
		int *kids = malloc(sizeof(int)*n);
		for(int i=0;i<n;i++) kids[i]=G->adj[v][i];
		g_sort_L = L;
		qsort(kids, n, sizeof(int), cmp_child_idx);
		for(int i=0;i<n;i++){
			print_map_rec(L, G, kids[i], depth+1, vis);
		}
		free(kids);
	}
	vis[v]=2;
}

// --- NEW: DFS mark reachable from root over j->i edges (dependents) ---
static void dfs_mark_from(int v, graph_t *G, char *need){
	if(need[v]) return;
	need[v] = 1;
	for(int t=0; t<G->adjn[v]; t++){
		int w = G->adj[v][t];
		dfs_mark_from(w, G, need);
	}
}

static int cmd_map(const char *rootname){
	unit_list_t L; if(load_all_units(&L)!=0){ info("No units"); return 1; }

	const char *root = (rootname && *rootname) ? rootname : "rootinit";
	int r = find_unit(&L, root);
	if(r<0){
		info("Root unit not found: %s", root);
		for(int i=0;i<L.count;i++) free_unit(&L.items[i]); free(L.items);
		return 1;
	}

	graph_t G; build_graph(&L, &G);
	char *vis = calloc(L.count, 1);

	printf("Start map (root: %s)\n", root);
	print_map_rec(&L, &G, r, 0, vis);

	free(vis);
	graph_free(&G, L.count);
	for(int i=0;i<L.count;i++) free_unit(&L.items[i]); free(L.items);
	return 0;
}

/** Create a once-per-boot guard using an exclusive lock file.
 *  @path /var/run/darwinctl.core.once
 *  @return 0 if this is the first run in current boot; -1 if already created or on error.
 *  @rationale /var/run is cleared at boot; simple, robust.
 */
static int boot_once_guard(void) {
	const char *lockpath = "/var/run/darwinctl.core.once";
	int fd = open(lockpath, O_CREAT | O_EXCL | O_WRONLY, 0644);
	if (fd < 0) {
		if (errno == EEXIST) {
			info("core_init: already booted in this session, skipping");
			return -1; 
		} else {
			info("core_init: lock open failed (%d): %s", errno, strerror(errno));
			return -1;
		}
	}
	dprintf(fd, "pid=%d time=%ld\n", (int)getpid(), (long)time(NULL));
	close(fd);
	return 0;
}


/** CLI: core_init.
 *  Behavior:
 *    - Enforce once-per-boot via boot_once_guard().
 *    - Load all units and topo sort.
 *    - Start 'rootinit' chain (root and all its dependents) in topological order.
 *    - Optionally start leftover autostart units (if used).
 *    - Write state index.
 *  @note This is typically invoked by launchd at boot.
 */
static int cmd_core_init(){
	if (boot_once_guard() != 0) {
		return 0;
	}
	
	unit_list_t L; 
	if(load_all_units(&L)!=0){ 
		info("No units"); 
		return 1; 
	}

	int N = 0; 
	int *ord = topo_order(&L, &N);
	if(!ord){ 
		info("Dependency cycle detected"); 
		goto done;
	}

	info("core_init: %d units", N);

	int r = find_unit(&L, "rootinit");
	if (r < 0) {
		info("Warning: rootinit not found");
	}

	graph_t G; 
	build_graph(&L, &G);
	char *need = calloc(L.count, 1);
	if (r >= 0) {
		dfs_mark_from(r, &G, need);
	}

	if (r >= 0) {
		for (int i = 0; i < N; i++) {
			int v = ord[i];
			if (need[v]) {
				start_unit(&L.items[v]);
			}
		}
	}

	for (int i = 0; i < N; i++) {
		int v = ord[i];
		if (need[v]) continue;                 // уже запущены на шаге 3
		if (strcmp(L.items[v].name, "rootinit") == 0) continue; // на всякий
		if (L.items[v].autostart) {
			start_unit(&L.items[v]);
		}
	}

	write_state_index(&L);

	// cleanup
	free(need);
	graph_free(&G, L.count);
done:
	if (ord) free(ord);
	for (int i = 0; i < L.count; i++) free_unit(&L.items[i]);
	free(L.items);
	return 0;
}

static int cmd_start(const char *name){
	unit_list_t L; 
	if(load_all_units(&L)!=0){ info("No units"); return 1; }

	int r = find_unit(&L, name);
	if(r<0){ 
		info("Unit not found: %s", name); 
		for(int k=0;k<L.count;k++) free_unit(&L.items[k]); 
		free(L.items); 
		return 1; 
	}

	int N=0; 
	int *ord = topo_order(&L, &N);
	if(!ord){
		info("Dependency cycle detected");
		for(int k=0;k<L.count;k++) free_unit(&L.items[k]); 
		free(L.items);
		return 1;
	}

	// 2) Построить граф j->i (как в map) и отметить всё, что достижимо от root
	graph_t G; 
	build_graph(&L, &G);
	char *need = calloc(L.count, 1);
	dfs_mark_from(r, &G, need);

	info("Start with dependencies from root: %s", L.items[r].name);
	for(int i=0;i<N;i++){
		int v = ord[i];
		if(need[v]){
			start_unit(&L.items[v]);
		}
	}

	// cleanup
	free(need);
	graph_free(&G, L.count);
	free(ord);
	for(int k=0;k<L.count;k++) free_unit(&L.items[k]); 
	free(L.items);
	return 0;
}

// ---------- main ----------
static void usage(){
	printf("darwinctl - minimal user-space init for macOS\n");
	printf("Usage:\n");
	printf("  darwinctl core_init\n");
	printf("  darwinctl refresh\n");
	printf("  darwinctl start <unit>\n");
	printf("  darwinctl stop <unit>\n");
	printf("  darwinctl edit <unit>\n");
	printf("  darwinctl map [rootunit]\n"); 
}

int main(int argc, char **argv){
	if(argc<2){ usage(); return 1; }
	const char *cmd=argv[1];
	if(strcmp(cmd,"core_init")==0) return cmd_core_init();
	if(strcmp(cmd,"refresh")==0) return cmd_refresh();
	if(strcmp(cmd,"start")==0 && argc>=3) return cmd_start(argv[2]);
	if(strcmp(cmd,"stop")==0 && argc>=3) return cmd_stop(argv[2]);
	if(strcmp(cmd,"edit")==0 && argc>=3) return cmd_edit(argv[2]);
	if(strcmp(cmd,"map")==0) return cmd_map(argc>=3 ? argv[2] : NULL); // <-- NEW
	usage(); return 1;
}