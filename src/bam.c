#include "system.h"
#include "sccs.h"
#include "bkd.h"
#include "tomcrypt.h"
#include "range.h"
#include "bam.h"

/*
 * TODO:
 *  - change on-disk MDBM to support adler32 per page
 *  - check error messages (bk get => 'get: no BAM_server for update.')
 *  - checkout:get shouldn't make multiple BAM_server connections
 *    resolve.c:copyAndGet(), check.c, others?
 *  * clean shouldn't be allowed to run on a BAM server
 *  - check needs a progress bar
 *  - check show list data files not linked in the index
 *  - BAM gone-file support
 *  - work on errors when BAM repos are used by old clients
 *  - keysync should sync BP data for non-shared server case
 *   ( no need to send all data)
 */

/*
 * Theory of operation
 *
 * BAM stores binary deltas by hash under BitKeeper/BAM in
 * uncompressed files.  The binary data is detached from the revision
 * history so that clones and pulls can avoid transfering all the
 * data.
 *
 * The files are stored in
 *	BitKeeper/BAM/xy/xyzabc...d1	// data
 *	BitKeeper/BAM/index.db
 *	// "hash key md5rootkey cset_md5rootkey" => "xy/xyzabc...d1"
 */

private	char*	hash2path(project *p, char *hash);
private int	bp_insert(project *proj, char *file, char *keys, int canmv);
private	int	uu2bp(sccs *s);

#define	INDEX_DELETE	"----delete----"

/*
 * Allocate s->gfile into the BAM pool and update the delta stuct with
 * the correct information.
 */
int
bp_delta(sccs *s, delta *d)
{
	char	*keys;
	int	rc;
	char	*p = aprintf("%s/BitKeeper/log/BAM", proj_root(s->proj));

	unless (exists(p)) touch(p, 0664);
	free(p);
	if (bp_hashgfile(s->gfile, &d->hash, &d->sum)) return (-1);
	s->dsum = d->sum;
	keys = sccs_prsbuf(s, d, 0, BAM_DSPEC);
	rc = bp_insert(s->proj, s->gfile, keys, 0);
	free(keys);
	if (rc) return (-1);
	d->added = size(s->gfile);
	unless (d->added) d->added = 1; /* added = 0 is bad */
	d->same = d->deleted = 0;
	return (0);
}

/*
 * Diff a gfile with the contents of a BAM delta.
 * Try to avoid reading any big files if possible.
 *
 * Return:
 *   0 no diffs
 *   1 diffs
 *   2 error
 */
int
bp_diff(sccs *s, delta *d, char *gfile)
{
	char	*dfile;
	int	same;

	unless (dfile = bp_lookup(s, d)) return (2);
	if (size(dfile) != size(gfile)) {
		free(dfile);
		return (1);
	}
	same = sameFiles(gfile, dfile);
	free(dfile);
	return (same ? 0 : 1);
}

/*
 * Find the BAM delta where the data actually changed.
 */
delta *
bp_fdelta(sccs *s, delta *d)
{
	while (d && !d->hash) d = d->parent;
	unless (d && d->parent) {
		fprintf(stderr,
		    "BAM: unable to find BAM delta in %s\n", s->gfile);
		return (0);
	}
	return (d);
}

/*
 * sccs_get() code path for BAM
 */
int
bp_get(sccs *s, delta *din, u32 flags, char *gfile)
{
	delta	*d;
	char	*dfile, *p;
	u32	sum;
	MMAP	*m;
	int	n, fd, ok;
	int	rc = -1;
	mode_t	mode;
	char	hash[10];

	d = bp_fdelta(s, din);
	unless (dfile = bp_lookup(s, d)) return (EAGAIN);
	unless (m = mopen(dfile, "rb")) {
		free(dfile);
		return (-1);
	}
	if (getenv("_BK_FASTGET")) {
		sum = strtoul(basenm(dfile), 0, 16);
	} else {
		if (getenv("_BK_FAKE_HASH")) {
			sum = strtoul(getenv("_BK_FAKE_HASH"), 0, 16);
		} else {
			sum = adler32(0, m->mmap, m->size); // XXX hash dfile?
			if (p = getenv("_BP_HASHCHARS")) {
				sprintf(hash, "%08x", sum);
				hash[atoi(p)] = 0;
				sum = strtoul(hash, 0, 16);
			}
		}
	}
	ok = (sum == strtoul(d->hash, 0, 16));
	unless (ok) {
		p = strchr(d->hash, '.');
		*p = 0;
		fprintf(stderr,
		    "crc mismatch in %s: %08x vs %s\n", s->gfile, sum, d->hash);
		*p = '.';
	}
	if (ok || (flags & GET_FORCE)) {
		if ((flags & (GET_EDIT|PRINT)) ||
		    !proj_configbool(s->proj, "BAM_hardlinks") ||
		    (link(dfile, gfile) != 0)) {
			unless (flags & PRINT) unlink(gfile);
			assert(din->mode);
			if ((flags & PRINT) && streq(gfile, "-")) {
				fd = 1;
			} else {
				assert(din->mode & 0200);
				(void)mkdirf(gfile);
				fd = open(gfile, O_WRONLY|O_CREAT, din->mode);
			}
			if (fd == -1) {
				perror(gfile);
				goto out;
			}
			if (writen(fd, m->mmap, m->size) != m->size) {
				unless (flags & PRINT) {
					perror(gfile);
					unlink(gfile);
					close(fd);
				}
				goto out;
			}
			close(fd);
		}
		unless (flags & PRINT) {
			mode = din->mode;
			unless (flags & GET_EDIT) mode &= ~0222;
			if (chmod(gfile, mode)) {
				fprintf(stderr,
				    "bp_get: cannot chmod %s\n", gfile);
				perror(gfile);
				goto out;
			}
		}
		rc = 0;
	}
	unless (n = m->size) n = 1;	/* zero is bad */
	s->dsum = sum & 0xffff;
	s->added = n;
	s->same = s->deleted = 0;
out:	mclose(m);
	free(dfile);
	return (rc);
}

/*
 * Calculate the hash for a gfile and return it has a malloced string in
 * hashp.  Also return the deltas checksum in sump.
 *
 * Both an alder32 and md5sum are calculated and the resulting string is
 * of the form "adler32.md5sum".  For example:
 *	03aa0108.Yhv0mKV2EroqcariHTKnog
 *
 * Function returns non-zero if gfile can't be read.
 */
int
bp_hashgfile(char *gfile, char **hashp, sum_t *sump)
{
	int	fd, i;
	u32	sum = 0;
	char	*p;
	int	hdesc = register_hash(&md5_desc);
	unsigned long	b64len;
	hash_state	md;
	char	buf[8<<10];	/* what is the optimal length? */

	if ((fd = open(gfile, O_RDONLY, 0)) < 0) return (-1);
	hash_descriptor[hdesc].init(&md);
	while ((i = read(fd, buf, sizeof(buf))) > 0) {
		sum = adler32(sum, buf, i);

		hash_descriptor[hdesc].process(&md, buf, i);
	}
	hash_descriptor[hdesc].done(&md, buf);
	close(fd);

	*hashp = malloc(36);
	if (p = getenv("_BK_FAKE_HASH")) {
		strcpy(*hashp, p);
	} else {
		sprintf(*hashp, "%08x", sum);
		if (p = getenv("_BP_HASHCHARS")) (*hashp)[atoi(p)] = 0;
	}
	p = *hashp + strlen(*hashp);
	*p++ = '.';
	b64len = 64;
	base64_encode(buf, hash_descriptor[hdesc].hashsize, p, &b64len);
	for (; *p; p++) {
		if (*p == '/') *p = '-';	/* dash */
		if (*p == '+') *p = '_';	/* underscore */
		if (*p == '=') {
			*p = 0;
			break;
		}
	}
	*sump = (sum_t)(strtoul(*hashp, 0, 16) & 0xffff);
	return (0);
}

/*
 * Insert the named file into the BAM pool, checking to be sure that we do
 * not have another file with the same hash but different data.
 * If canmv is set then move instead of copy.
 */
private int
bp_insert(project *proj, char *file, char *keys, int canmv)
{
	char	*base, *p;
	MDBM	*db;
	int	j;
	char	buf[MAXPATH];
	char	tmp[MAXPATH];

	base = hash2path(proj, keys);
	p = buf + sprintf(buf, "%s", base);
	free(base);
	for (j = 1; ; j++) {
		sprintf(p, ".d%d", j);
// ttyprintf("TRY %s\n", buf);

		unless (exists(buf)) break;

		/*
		 * If the data matches then we have two deltas which point at
		 * the same data.
		 */
		if (sameFiles(file, buf)) goto domap;//XXX bug
	}
	/* need to insert new entry */
	mkdirf(buf);

	unless (canmv) {
		sprintf(tmp, "%s.tmp", buf);
		if (fileCopy(file, tmp)) goto nofile;
		file = tmp;
	}
	if (rename(file, buf) && fileCopy(file, buf)) {
nofile:		fprintf(stderr, "BAM: insert to %s failed\n", buf);
		unless (canmv) unlink(buf);
		return (1);
	}
domap:
	/* move p to path relative to BAM dir */
	while (*p != '/') --p;
	--p;

	while (*p != '/') --p;
	*p++ = 0;

	db = proj_BAMindex(proj, 1);
	assert(db);
	j = mdbm_store_str(db, keys, p, MDBM_REPLACE);
	bp_logUpdate(keys, p);
	assert(!j);
	return (0);
}

/*
 * Given keys (":BAMHASH: :KEY: :MD5ROOTKEY: :CSET_MD5ROOTKEY:") return the .d
 * file in the BAM pool that contains that data or null if the
 * data doesn't exist.
 * The pathname returned is malloced and needs to be freed.
 */
char *
bp_lookupkeys(project *proj, char *keys)
{
	MDBM	*db;
	char	*p, *t;

	unless (db = proj_BAMindex(proj, 0)) return (0);
	if (t = mdbm_fetch_str(db, keys)) {
		p = hash2path(proj, 0);
		t = aprintf("%s/%s", p, t);
		free(p);
		unless (exists(t)) {
			free(t);
			t = 0;
		}
	}
	return (t);
}


/*
 * Find the pathname to the BAM file for the gfile.
 */
char *
bp_lookup(sccs *s, delta *d)
{
	char	*keys;
	char	*ret = 0;

	d = bp_fdelta(s, d);
	keys = sccs_prsbuf(s, d, 0, BAM_DSPEC);
	ret = bp_lookupkeys(s->proj, keys);
	free(keys);
	return (ret);
}

/*
 * Given the adler32 hash of a gfile returns the full pathname to that
 * file in a repo's BAM pool.
 * if hash==0 just return the path to the BAM pool
 */
private char *
hash2path(project *proj, char *hash)
{
	char    *p, *t;
	int     i;
	char    bam[MAXPATH];

	unless (p = proj_root(proj)) return (0);
	strcpy(bam, p);
	if ((p = strrchr(bam, '/')) && patheq(p, "/RESYNC")) *p = 0;
	strcat(bam, "/BitKeeper/BAM");
	if (hash) {
		i = strlen(bam);
		if (t = strchr(hash, '.')) *t = 0;
		sprintf(bam+i, "/%c%c/%s", hash[0], hash[1], hash);
		if (t) *t = '.';
	}
	return (strdup(bam));
}

/*
 * called from slib.c:get_bp() when a BAM file can't be found.
 */
int
bp_fetch(sccs *s, delta *din)
{
	FILE	*f;
	char	*repoID, *url, *cmd, *keys;

	/* find the repo_id of my server */
	if (bp_serverID(&repoID)) return (-1);
	unless (repoID) return (0);	/* no need to update myself */
	free(repoID);
	url = proj_configval(0, "BAM_server");
	assert(url);

	unless (din = bp_fdelta(s, din)) return (-1);

	/*
	 * No recursion, we're already remoted.
	 * No stdin buffering, it's just one key.
	 */ 
	cmd =
	    aprintf("bk -q@'%s' -zo0 -Lr sfio -oqB - | bk -R sfio -iqB -", url);
	f = popen(cmd, "w");
	free(cmd);
	assert(f);
	keys = sccs_prsbuf(s, din, 0, BAM_DSPEC);
	fprintf(f, "%s\n", keys);
	free(keys);
	if (pclose(f)) {
		fprintf(stderr, "bp_fetch: failed to fetch delta for %s\n",
		    s->gfile);
		return (-1);
	}
	return (0);
}

/*
 * Log an update to the BAM index.db to the file
 * BitKeeper/log/BAM.log
 * Each entry in that file looks like:
 *  <index key> PATH hash
 *
 * val == 0 is a delete.
 */
int
bp_logUpdate(char *key, char *val)
{
	FILE	*f;
	char	buf[MAXPATH];

	unless (val) val = INDEX_DELETE;
	strcpy(buf, proj_root(0));
	if (proj_isResync(0)) concat_path(buf, buf, RESYNC2ROOT);
	strcat(buf, "/BitKeeper/log/BAM.index");
	unless (f = fopen(buf, "a")) return (-1);
	sprintf(buf, "%s %s", key, val);
	fprintf(f, "%s %08x\n", buf, adler32(0, buf, strlen(buf)));
	fclose(f);
	return (0);
}

/*
 * Copy all data local to the BAM pool to my server.
 * XXX we ignore tiprev for now.
 */
int
bp_updateServer(char *tiprev, int all, int quiet)
{
	char	*p, *url, *tmpkeys, *tmpkeys2;
	char	*cmd;
	char	*repoID;
	int	rc;
	u64	todo = 0;
	FILE	*in, *out;
	MDBM	*bp;
	kvpair	kv;
	char	buf[MAXLINE];

	unless (bp_hasBAM()) return (0);

	putenv("BKD_DAEMON="); /* allow new bkd connections */

	/* find the repo_id of my server */
	if (bp_serverID(&repoID)) return (-1);
	unless (repoID) return (0); /* no need to update myself */
	free(repoID);
	url = proj_configval(0, "BAM_server");
	assert(url);

	tmpkeys = bktmp(0, 0);

	if (all) {
		unless (bp = proj_BAMindex(0, 0)) {
			unlink(tmpkeys);
			free(tmpkeys);
			return (1);
		}
		out = fopen(tmpkeys, "w");
		for (kv = mdbm_first(bp); kv.key.dsize; kv = mdbm_next(bp)) {
			sprintf(buf, "BitKeeper/BAM/%s", kv.val.dptr);
			fprintf(out, "|%u|%s\n", size(buf), kv.key.dptr);
		}
		fclose(out);
	} else {
		/* find local only bp deltas */
		cmd = aprintf("bk changes -qBv -L -nd'"
		    "$if(:BAMHASH:){|:SIZE:|:BAMHASH: :KEY: :MD5KEY|1.0:}' "
		    "'%s' > '%s'",
		    url, tmpkeys);
		if (system(cmd)) {
			unlink(tmpkeys);
			free(tmpkeys);
			free(cmd);
			return (-1);
		}
		free(cmd);
	}
	if (size(tmpkeys) == 0) { /* no keys to sync? just quit */
		unlink(tmpkeys);
		free(tmpkeys);
		return (0);
	}
// ttyprintf("sending %u bytes\n", (u32)size(tmpkeys));
	tmpkeys2 = bktmp(0, 0);
	/* For now, we are not recursing to a chained server */
	p = aprintf("-q@%s", url);
	rc = sysio(tmpkeys, tmpkeys2, 0,
	    "bk", p, "-Lr", "-Bstdin", "havekeys", "-B", "-", SYS);
	free(p);
	unless (rc || (sizeof(tmpkeys2) == 0)) {
		unless (quiet) {
			fprintf(stderr, "Updating BAM files at %s\n", url);
		}
		in = fopen(tmpkeys2, "r");
		out = fopen(tmpkeys, "w");
		while (fnext(buf, in)) {
			p = strchr(buf+1, '|');
			assert(p);
			*p++ = 0;
			/* limits us to 4GB/file, strtoull isn't portable */
			todo += strtoul(buf+1, 0, 10);
			fputs(p, out);
		}
		fclose(in);
		fclose(out);

		/* No recursion, we're looking for our files */
		cmd = aprintf("bk sfio -o%srBb%s - < '%s' |"
		    "bk -q@'%s' -z0 -Lw sfio -iqB -", 
		    quiet ? "q" : "", psize(todo), tmpkeys, url);
// ttyprintf("CMD=%s\n", cmd);
		rc = system(cmd);
		free(cmd);
	}
	unlink(tmpkeys);
	free(tmpkeys);
	unlink(tmpkeys2);
	free(tmpkeys2);
	return (rc);
}

/*
 * Find the repo_id of the BAM server and returns it as a malloc'ed
 * string in *id.
 * Returns non-zero if we failed to determine the repo_id of the server.
 */
int
bp_serverID(char **id)
{
	char	*cache, *p, *url;
	char	*ret = 0;
	FILE	*f;
	char	cfile[MAXPATH];
	char	buf[MAXLINE];

	assert(id);
	*id = 0;
	unless ((url = proj_configval(0,"BAM_server")) && *url) return (0);

	strcpy(cfile, proj_root(0));
	if (proj_isResync(0)) concat_path(cfile, cfile, RESYNC2ROOT);
	concat_path(cfile, cfile, "BitKeeper/log/BP_SERVER");
	if (cache = loadfile(cfile, 0)) {
		if (p = strchr(cache, '\n')) {
			*p++ = 0;
			chomp(p);
			if (streq(url, cache)) ret = strdup(p);
		}
		free(cache);
		if (ret) goto out;
	}
	sprintf(buf, "bk -q@'%s' id -r", url);
	f = popen(buf, "r");
	fnext(buf, f);
	chomp(buf);
	if (pclose(f)) {
		fprintf(stderr, "Failed to contact BAM server at '%s'\n",
		    url);
		return (-1);
	}
	ret = strdup(buf);
	if (f = fopen(cfile, "w")) {
		fprintf(f, "%s\n%s\n", url, ret);
		fclose(f);
	}
out:	if (streq(proj_repoID(0), ret)) {
		free(ret);
		ret = 0;
	}
	*id = ret;
	return (0);
}

int
bp_sharedServer(void)
{
	char	*local_repoID;	/* repoID of local bp_server (may be me) */
	char	*remote_repoID;	/* repoID of remote bp_server */
	int	rc;
	char	*p = getenv("_BK_IN_BKD");
	int	inbkd = p && *p;

	unless (bp_hasBAM()) {
		//ttyprintf("no bp data, shared\n");
		return (1);
	}
	if (bp_serverID(&local_repoID)) {
		//ttyprintf("unable to fetch serverID, shared\n");
		return (1);
	}
	unless (local_repoID) local_repoID = strdup(proj_repoID(0));
	remote_repoID =
	    getenv(inbkd ? "BK_BAM_SERVER" : "BKD_BAM_SERVER");
	unless (remote_repoID) {
		/*
		 * remote side doesn't support BAM, other error checks
		 * will catch this.
		 */
		//ttyprintf("no remote_id, shared\n");
		free(local_repoID);
		return (1);
	}

	/* Do we both use the same server? If so then we are done. */
	rc = streq(local_repoID, remote_repoID);
	free(local_repoID);
	//ttyprintf("id's match=%d\n", rc);
	return (rc);
}

int
bp_hasBAM(void)
{
	assert(exists(BKROOT));
	if (proj_isResync(0)) {
		return (exists("../BitKeeper/log/BAM"));
	} else {
		return (exists("BitKeeper/log/BAM"));
	}
}

/* make local repository contain BAM data for all deltas */
private int
bam_pull_main(int ac, char **av)
{
	int	rc, c;
	char	*p;
	char	**cmds = 0;

	while ((c = getopt(ac, av, "a")) != -1) {
		switch (c) {
		    case 'a':
			fprintf(stderr,
			    "BAM pull: -a not implemeneted.\n");
			return (1);
		    default:
			system("bk help -s BAM");
			return (1);
		}
	}

	if (bp_serverID(&p)) return (1);
	unless (p) return (0);
	free(p);

	/* list of all BAM deltas */
	cmds = addLine(cmds, aprintf("bk changes -Bv -nd'" BAM_DSPEC "'"));

	/* reduce to list of deltas missing locally, no recursion. */
	cmds = addLine(cmds, strdup("bk havekeys -B -"));

	/* request deltas from server, no recursion (yet) */
	cmds = addLine(cmds, aprintf("bk -q@'%s' -zo0 -Lr -Bstdin sfio -oqB -",
	    proj_configval(0, "BAM_server")));

	/* unpack locally */
	cmds = addLine(cmds, strdup("bk sfio -irB -"));

	rc = spawn_filterPipeline(cmds);
	freeLines(cmds, free);
	return (rc);
}

/* update BAM server with any BAM data committed locally */
private int
bam_push_main(int ac, char **av)
{
	int	c;
	int	all = 0, quiet = 0;
	char	*tiprev = "+";

	while ((c = getopt(ac, av, "aqr;")) != -1) {
		switch (c) {
		    case 'a': all = 1; break;
		    case 'q': quiet = 1; break;
		    case 'r': tiprev = optarg; break;
		    default:
			system("bk help -s BAM");
			return (1);
		}
	}
	if (proj_cd2root()) {
		fprintf(stderr, "Not in a repository.\n");
		return (1);
	}
	return (bp_updateServer(tiprev, all, quiet));
}

/*
 * Remove any BAM data from the current repository that is not
 * used by any local deltas.
 */
private int
bam_clean_main(int ac, char **av)
{
	FILE	*f;
	int	i, j, c, dels;
	hash	*bpdeltas;	/* list of deltakeys to keep */
	hash	*dfiles;	/* list of data files to delete */
	MDBM	*db;		/* BAM index file */
	char	**fnames;	/* sorted list of dfiles */
	hash	*renames = 0;	/* list renamed data files */
	char	*p1, *p2;
	char	*cmd;
	int	check_server = 0, dryrun = 0;
	kvpair	kv;
	char	buf[MAXLINE];

	while ((c = getopt(ac, av, "an")) != -1) {
		switch (c) {
		    case 'a': check_server = 1; break;
		    case 'n': dryrun = 1; break;
		    default:
			system("bk help -s BAM");
			return (1);
		}
	}
	if (proj_cd2root()) {
		fprintf(stderr, "Not in a repository.\n");
		return (1);
	}
	if (sys("bk", "BAM", "check", "-Fq", SYS)) {
		fprintf(stderr,
		    "bk BAM clean: check failed, clean cancelled.\n");
		return (1);
	}

	/* save bp deltas in repo */
	cmd = strdup("bk -r prs -hnd'" BAM_DSPEC "'");
	if (check_server) {
		/* remove deltas already in BAM server */
		p1 = cmd;
		if (bp_serverID(&p2)) return (1);
		unless (p2) {
			fprintf(stderr,
			    "bk BAM clean: No BAM_server set\n");
			free(p1);
			return (1);
		}
		free(p2);
		p2 = proj_configval(0, "BAM_server");
		/* No recursion, we just want that server's list */
		cmd = aprintf("%s | "
			"bk -q@'%s' -Lr -Bstdin havekeys -B -", p1, p2);
		free(p1);
	}
	bpdeltas = hash_new(HASH_MEMHASH);
	f = popen(cmd, "r");
	free(cmd);
	assert(f);
	while (fnext(buf, f)) {
		chomp(buf);
		hash_storeStr(bpdeltas, buf, 0);
	}
	if (pclose(f)) {
		fprintf(stderr, "bk BAM clean: failed to contact server\n");
		return (1);
	}

	chdir("BitKeeper/BAM");

	/* get list of data files */
	dfiles = hash_new(HASH_MEMHASH);
	f = popen("bk _find . -type f -name '*.d*'", "r");
	assert(f);
	while (fnext(buf, f)) {
		chomp(buf);
		unless (buf[2] == '/') continue;	/* not index.db */
		hash_insertStr(dfiles, buf, "");
	}
	if (pclose(f)) assert(0); /* shouldn't happen */

	/* walk all bp deltas */
	db = proj_BAMindex(0, 1);
	assert(db);
	fnames = 0;
	EACH_KV(db) {
		/* keep keys we still need */
		if (hash_fetchStr(bpdeltas, kv.key.dptr)) {
			p1 = hash_fetchStr(dfiles, kv.val.dptr);
			if (p1) {
				*p1 = '1';	/* keep the file */
				continue;	/* keep the key */
			} else {
				fprintf(stderr,
				"BAM clean: data for key '%s' missing,\n"
				"\tdeleting key.\n", kv.key.dptr);
			}
		}

		/*
		 * This is a key we don't need, but we don't know if we can
		 * delete the datafile yet because it might be pointed at
		 * by other delta keys.
		 */
		if (dryrun) {
			fprintf(stderr,
			    "BAM clean: would remove %s\n", kv.key.dptr);
		} else {
			fnames = addLine(fnames, strdup(kv.key.dptr));
		}
	}
	EACH(fnames) {
		mdbm_delete_str(db, fnames[i]);
		bp_logUpdate(fnames[i], 0);
	}
	freeLines(fnames, free);
	hash_free(bpdeltas);

	/*
	 * now we can walk the datafiles and delete any that are not
	 * referenced by a key in the index.
	 */
	fnames = 0;
	EACH_HASH(dfiles) fnames = addLine(fnames, dfiles->kptr);
	sortLines(fnames, 0);
	EACH(fnames) {
		if ((p1 = hash_fetchStr(dfiles, fnames[i])) && *p1) continue;

		unlink(fnames[i]); /* delete data file */
		dels = 1;

		/* see if the next data files need to be renamed */
		strcpy(buf, fnames[i]);
		p1 = strrchr(buf, '.');
		assert(p1);
		p1 += 2;
		c = atoi(p1);
		for (j = 1; ; j++) {
			sprintf(p1, "%d", c + j);

			unless (i+1 < LSIZ(fnames) && fnames[i+1]) break;
			unless (streq(buf, fnames[i+1])) break;

			unless ((p2 = hash_fetchStr(dfiles, buf)) && *p2) {
				unlink(buf);
				++dels;
			} else {
				/* going to rename */
				sprintf(p1, "%d", c + j - dels);
				unless (renames) {
					renames = hash_new(HASH_MEMHASH);
				}
				hash_storeStr(renames, fnames[i+1], buf);
			}
			++i;
		}
	}
	freeLines(fnames, 0);
	hash_free(dfiles);

	if (renames) {
		/* update index file */
		EACH_KV(db) {
			if (p1 = hash_fetchStr(renames, kv.val.dptr)) {
				/* data will always fit */
				strcpy(kv.val.dptr, p1);
				bp_logUpdate(kv.key.dptr, p1);
			}
		}

		/* do renames */
		fnames = 0;
		EACH_HASH(renames) fnames = addLine(fnames, renames->kptr);
		sortLines(fnames, 0);
		EACH(fnames) {
			p1 = hash_fetchStr(renames, fnames[i]);
			rename(fnames[i], p1);
		}
		freeLines(fnames, 0);
		hash_free(renames);
	}
	return (0);
}

int
bp_check_hash(char *want, char ***missing, int fast)
{
	char	*dfile;
	char	*hval;
	char	*p;
	sum_t	sum;
	int	rc = 0;

	unless (dfile = bp_lookupkeys(0, want)) {
		if (missing) *missing = addLine(*missing, strdup(want));
		return (0);
	}
	p = strchr(want, ' ');	/* just the hash */
	assert(p);
	*p = 0;
	unless (fast) {
		if (bp_hashgfile(dfile, &hval, &sum)) {
			assert(0);	/* shouldn't happen */
		}
		unless (streq(want, hval)) {
			fprintf(stderr,
			    "BAM file %s has a hash mismatch.\n"
			    "want: %s got: %s\n", dfile, want, hval);
			rc = 1;
		}
		free(hval);
	}
	if (strtoul(want, 0, 16) != strtoul(basenm(dfile), 0, 16)) {
		fprintf(stderr,
		    "BAM datafile stored under wrong filename: %s\n",
		    dfile);
		rc = 1;
	}
	*p = ' ';
	free(dfile);
	return (rc);
}

int
bp_check_findMissing(int quiet, char **missing)
{
	FILE	*f;
	int	i, rc = 0;
	char	*p;
	char	*tmp;
	char	buf[MAXLINE];

	if (emptyLines(missing)) return (0);
	if (!bp_serverID(&p) && p) {
		unless (quiet) {
			fprintf(stderr,
			    "Looking for %d missing files in %s\n",
			    nLines(missing),
			    proj_configval(0, "BAM_server"));
		}
		free(p);

		tmp = bktmp(0, 0);
		/* no recursion, we are remoted to the server already */
		p = aprintf("bk -q@'%s' -Lr -Bstdin havekeys -B - > '%s'",
		    proj_configval(0, "BAM_server"), tmp);
		f = popen(p, "w");
		free(p);
		assert(f);
		EACH(missing) fprintf(f, "%s\n", missing[i]);
		if (pclose(f)) {
			fprintf(stderr,
			    "Failed to contact BAM_server at %s\n",
			    proj_configval(0, "BAM_server"));
			rc = 1;
		} else {
			EACH(missing) {	/* clear array in place */
				free(missing[i]);
				missing[i] = 0;
			}
			f = fopen(tmp, "r");
			while (fnext(buf, f)) {
				chomp(buf);
				missing = addLine(missing, strdup(buf));
			}
			fclose(f);
		}
		unlink(tmp);
		free(tmp);
	}
	unless (emptyLines(missing)) {
		fprintf(stderr,
		   "Failed to locate BAM data for the following deltas:\n");
		EACH(missing) fprintf(stderr, "\t%s\n", missing[i]);
		rc = 1;
	}
	return (rc);
}


/*
 * Validate the checksums and metadata for all BAM data
 *  delta checksum matches filename and file contents
 *  all BAM deltas have data here or in BAM_server
 *
 * TODO:
 *  - check permissions
 *  - index.db matches logfile
 */
private int
bam_check_main(int ac, char **av)
{
	int	rc = 0, quiet = 0, fast = 0, n = 0, i;
	FILE	*f;
	char	**missing = 0;
	char	buf[MAXLINE];

	if (proj_cd2root()) {
		fprintf(stderr, "Not in a repository.\n");
		return (1);
	}
	while ((i = getopt(ac, av, "Fq")) != -1) {
		switch (i) {
		    case 'F': fast = 1; break;
		    case 'q': quiet = 1; break;
		    default:
			system("bk help -s BAM");
			return (1);
		}
	}

	/* load BAM deltas and hashs */
	f = popen("bk -r prs -hnd'" BAM_DSPEC "'", "r");
	assert(f);
	while (fnext(buf, f)) {
		unless (quiet) fprintf(stderr, "%d\r", ++n);
		chomp(buf);

		if (bp_check_hash(buf, &missing, fast)) rc = 1;
	}
	pclose(f);
	unless (quiet) fprintf(stderr, "\n");

	/*
	 * if we are missing some data make sure it is not covered by the
	 * BAM server before we complain.
	 */
	unless ((rc |= bp_check_findMissing(quiet, missing)) || quiet) {
		fprintf(stderr, "All BAM data was found, check passed.\n");
	}
	return (rc);
}

/*
 * Examine the files in a directory and add any files that have the
 * right hash to be data missing from my BAM pool back to the BAM pool.
 */
private int
bam_repair_main(int ac, char **av)
{
	char	*dir, *cmd, *hval, *p;
	int	i, c, quiet = 0;
	MDBM	*db = 0, *missing;
	FILE	*f;
	sum_t	sum;
	char	buf[MAXLINE];
	char	key[100];

	while ((c = getopt(ac, av, "q")) != -1) {
		switch (c) {
		    case 'q': quiet = 1; break;
		    default:
			system("bk help -s BAM");
			return (1);
		}
	}
	unless (av[optind] && !av[optind+1]) {
		fprintf(stderr, "usage: bk BAM repair DIR\n");
		return (1);
	}
	dir = fullname(av[optind]);
	if (proj_cd2root()) {
		fprintf(stderr, "Not in a repository.\n");
		return (1);
	}
	db = proj_BAMindex(0, 0);	/* ok if db is null */
	missing = mdbm_mem();

	/* save the hash for all the bp deltas we are missing */
	f = popen("bk changes -Bv -nd'" BAM_DSPEC "'", "r");
	assert(f);
	while (fnext(buf, f)) {
		chomp(buf);
		if (db && mdbm_fetch_str(db, buf)) continue;

		/*
		 * buf contains: adler.md5 key md5rootkey
		 * and what we want is 
		 *	missing{adler.md5} = "adler.md5 key md5rootkey"
		 * But we have to handle the case that more than one delta
		 * wants this hash, doesn't happen often but it happens.
		 * So if we try the store and the slot is taken, then
		 * try again with "adler.md5.%d" until it goes in.
		 */
		p = strchr(buf, ' ');
		*p = 0;
		strcpy(key, buf);
		*p = ' ';
		if (mdbm_store_str(missing, key, buf, MDBM_INSERT)) {
			for (i = 0; ; i++) {
				p = aprintf("%s.%d", key, i);
				c = mdbm_store_str(missing, p, buf,MDBM_INSERT);
				free(p);
				unless (c) break;
			}
		}
	}
	pclose(f);

	/* hash all files in directory ... */
	cmd = aprintf("bk _find '%s' -type f", dir);
	f = popen(cmd, "r");
	assert(f);
	while (fnext(buf, f)) {
		chomp(buf);

		if (bp_hashgfile(buf, &hval, &sum)) continue;
		unless (p = mdbm_fetch_str(missing, hval)) {
			free(hval);
			continue;
		}

		/* found a file we are missing */
		for (i = -1; ; i++) {
			unless (i == -1) {
				sprintf(key, "%s.%d", hval, i);
				unless (p = mdbm_fetch_str(missing, key)) {
					break;
				}
			}
			unless (quiet) printf("Inserting %s for %s\n", buf, p);
			if (bp_insert(0, buf, p, 0)) {
				fprintf(stderr,
				"BAM repair: failed to insert %s for %s\n",
				buf, p);
			}
			/* don't reinsert this key again */
			mdbm_delete_str(missing, key);
		}
	}
	mdbm_close(missing);
	return (0);
}

/* check that the log matches the index and vice versa */
int
bam_index_main(int ac, char **av)
{
	if (proj_cd2root()) {
		fprintf(stderr, "No repo root?\n");
		return (1);
	}
	return (bp_index_check(0));
}

private int
load_logfile(MDBM *m, FILE *f)
{
	char	*crc, *file;
	u32	aWant, aGot;
	char	buf[MAXLINE];

	while (fnext(buf, f)) {
		crc = strrchr(buf, ' ');
		assert(crc);
		*crc++ = 0;
		aWant = strtoul(crc, 0, 16);
		aGot = adler32(0, buf, strlen(buf));
		unless (aGot == aWant) {
			fprintf(stderr, "Skipping %s\n", buf);
			continue;
		}
		file = strrchr(buf, ' ');
		assert(file);
		*file++ = 0;
		if (streq(file, INDEX_DELETE)) {
			mdbm_delete_str(m, buf);
		} else {
			mdbm_store_str(m, buf, file, MDBM_REPLACE);
		}
	}
	return (0);
}


int
bp_index_check(int quiet)
{
	MDBM	*m, *i;
	FILE	*f;
	char	*p;
	int	log = 0, index = 0, missing = 0, mismatch = 0;
	kvpair	kv;

	i = proj_BAMindex(0, 0);
	f = fopen("BitKeeper/log/BAM.index", "r");

	if (!i && !f) return (0);
	unless (i && f) {
		fprintf(stderr, "No BAM.index?\n");
		return (1);
	}
	m = mdbm_mem();
	load_logfile(m, f);
	fclose(f);
	for (kv = mdbm_first(m); kv.key.dsize; kv = mdbm_next(m)) {
		log++;
		unless (p = mdbm_fetch_str(i, kv.key.dptr)) {
			fprintf(stderr, "Not found: %s\n", kv.key.dptr);
			missing++;
		} else unless (streq(p, kv.val.dptr)) {
			fprintf(stderr, "Mismatch: %s: %s != %s\n",
			    kv.key.dptr, kv.val.dptr, p);
			mismatch++;
		}
	}
	for (kv = mdbm_first(i); kv.key.dsize; kv = mdbm_next(i)) {
		index++;
		unless (p = mdbm_fetch_str(m, kv.key.dptr)) {
			fprintf(stderr,
			    "Extra: %s => %s\n", kv.key.dptr, kv.val.dptr);
		} else unless (streq(p, kv.val.dptr)) {
			fprintf(stderr, "Mismatch2: %s: %s != %s\n",
			    kv.key.dptr, kv.val.dptr, p);
		}
	}
	mdbm_close(m);
	unless (log == index) {
		fprintf(stderr,
		    "Count mismatch: %u in log, %u in index\n", log, index);
	}
	if (missing) {
		fprintf(stderr, "%d items missing in index.\n", missing);
	}
	if (mismatch) {
		fprintf(stderr, "%d items mismatched in index.\n", mismatch);
	}
	return (missing || mismatch || (log != index));
}

/* replay < logfile */
int
bam_replay_main(int ac, char **av)
{
	MDBM	*m;

	if (proj_cd2root()) {
		fprintf(stderr, "No repo root?\n");
		exit(1);
	}
	m = mdbm_open("BitKeeper/BAM/index.db", O_RDWR|O_CREAT, 0666, 4096);
	load_logfile(m, stdin);
	mdbm_close(m);
	return (0);
}

int
bam_sizes_main(int ac, char **av)
{
	sccs	*s;
	delta	*d;
	char	*name;
	int	errors = 0;
	u32	bytes;

	for (name = sfileFirst("BAM_sizes", &av[1], 0);
	    name; name = sfileNext()) {
		unless (s = sccs_init(name, 0)) continue;
		unless (HASGRAPH(s)) {
err:			sccs_free(s);
			errors |= 1;
			continue;
		}
		unless (HAS_GFILE(s)) {
			fprintf(stderr, "%s: not checked out.\n", s->gfile);
			goto err;
		}
		unless (d = bp_fdelta(s, sccs_top(s))) goto err;
		if (bytes = size(s->gfile)) {
			d->added = bytes;
			sccs_newchksum(s);
		}
		sccs_free(s);
	}
	if (sfileDone()) errors |= 2;
	return (errors);
}

char	**keys;

int
bam_convert_main(int ac, char **av)
{
	sccs	*s;
	char	*name;
	int	i, j, n, matched = 0, errors = 0;
	FILE	*in, *out;
	char	buf[MAXKEY * 2];

	unless (in = fopen("SCCS/s.ChangeSet", "r")) {
		fprintf(stderr, "Must be run at repo root\n");
		exit(1);
	}
	for (name = sfileFirst("BAM_convert", &av[1], 0);
	    name; name = sfileNext()) {
		unless (s = sccs_init(name, 0)) continue;
		unless (HASGRAPH(s)) {
			sccs_free(s);
			errors |= 1;
			continue;
		}
		unless ((s->encoding & E_DATAENC) == E_UUENCODE) {
			sccs_free(s);
			continue;
		}
		errors |= uu2bp(s);
		sccs_free(s);
	}
	if (sfileDone()) errors |= 2;
	unless (out = fopen("SCCS/x.ChangeSet", "w")) {
		exit(1);
	}
	fprintf(stderr, "Redoing ChangeSet entries ...\n");
	while (fnext(buf, in)) {
		fputs(buf, out);
		if (streq("\001T\n", buf)) break;
	}
	n = nLines(keys) / 2;
	while (fnext(buf, in)) {
		if (buf[0] == '\001') {
			fputs(buf, out);
			continue;
		}
		j = 0;
		EACH(keys) {
			if (streq(keys[i], buf)) {
				fputs(keys[i+1], out);
				j = i;
				matched++;
				break;
			}
			i++;
		}
		if (j) {
			fprintf(stderr, "Found %d of %d\r", matched, n);
			removeLineN(keys, j, free);
			removeLineN(keys, j, free);
		} else {
			fputs(buf, out);
		}
	}
	fprintf(stderr, "\n");
	fclose(in);
	fclose(out);
	EACH(keys) {
		fprintf(stderr, "%s", keys[i]);
	}
	rename("SCCS/s.ChangeSet", "s.ChangeSet");
	rename("SCCS/x.ChangeSet", "SCCS/s.ChangeSet");
	fprintf(stderr, "Redoing ChangeSet checksums ...\n");
	system("bk admin -z ChangeSet");
	system("bk checksum -f/ ChangeSet");
	fprintf(stderr, "Redoing ChangeSet ids ...\n");
	system("bk newroot -kB:");
	return (errors);
}

/*
 * Do surgery on this file to make it be a BAM file instead of
 * a uuencoded file.
 * - extract each version, bp_enter it, fix up the delta struct.
 * - switch the flags
 * - write the delta table
 * - kill the weave.
 */
private	int
uu2bp(sccs *s)
{
	delta	*d;
	FILE	*out;
	int	locked;
	char	*t;
	char	oldroot[MAXKEY], newroot[MAXKEY];
	char	key[MAXKEY];

	if (sccs_clean(s, 0)) return (4);
	d = sccs_ino(s);
	sccs_sdelta(s, d, oldroot);
	assert(d->random);
	t = aprintf("B:%s", d->random);
	free(d->random);
	d->random = t;
	sccs_sdelta(s, d, newroot);
	unless (locked = sccs_lock(s, 'z')) {
		fprintf(stderr, "BP can't get lock on %s\n", s->gfile);
		return (4);
	}
	for (d = s->table; d; d = d->next) {
		assert(d->type == 'D');
		if (sccs_get(s, d->rev, 0, 0, 0, 0, "-")) return (8);
		/*
		 * XXX - if this logic is wrong then we lose data.
		 * It may be worth it to go ahead and do the insert
		 * anyway, it's an extra tuple but the data will collapse.
		 * Review carefully.
		 */
		unless (d->added || d->deleted) {
			d->deleted = d->same = 0;
			d->added = size(s->gfile);
			unlink(s->gfile);
			unless (d->flags & D_CSET) continue;
			sccs_sdelta(s, d, key);
			keys = addLine(keys, aprintf("%s %s\n", oldroot, key));
			keys = addLine(keys, aprintf("%s %s\n", newroot, key));
			continue;
		}
		if (d->flags & D_CSET) {
			sccs_sdelta(s, d, key);
			keys = addLine(keys, aprintf("%s %s\n", oldroot, key));
		}
		if (bp_delta(s, d)) return (16);
		if (d->flags & D_CSET) {
			sccs_sdelta(s, d, key);
			keys = addLine(keys, aprintf("%s %s\n", newroot, key));
		}
		unlink(s->gfile);
	}
	unless (out = fopen(sccs_Xfile(s, 'x'), "w")) {
		fprintf(stderr, "BP: can't create %s: ", sccs_Xfile(s, 'x'));
err:		sccs_unlock(s, 'z');
		return (32);
	}
	s->encoding = E_BAM;
	if (delta_table(s, out, 0)) goto err;
	fseek(out, 0L, SEEK_SET);
	fprintf(out, "\001%c%05u\n", 'H', s->cksum);
	fclose(out);
	sccs_close(s);
	t = sccs_Xfile(s, 'x');
	if (rename(t, s->sfile)) {
		fprintf(stderr,
		    "BP: can't rename(%s, %s) left in %s\n", t, s->sfile, t);
		goto err;
	}
	sccs_setStime(s, 0);
	chmod(s->sfile, 0444);
	sccs_unlock(s, 'z');
	return (0);
}

int
bam_main(int ac, char **av)
{
	int	c, i;
	struct {
		char	*name;
		int	(*fcn)(int ac, char **av);
	} cmds[] = {
		{"pull", bam_pull_main },
		{"push", bam_push_main },
		{"clean", bam_clean_main },
		{"check", bam_check_main },
		{"repair", bam_repair_main },
		{"index", bam_index_main },
		{"replay", bam_replay_main },
		{"sizes", bam_sizes_main },
		{"convert", bam_convert_main },
		{0, 0}
	};

	while ((c = getopt(ac, av, "")) != -1) {
		switch (c) {
		    default:
usage:			system("bk help -s BAM");
			return (1);
		}
	}
	unless (av[optind]) goto usage;
	for (i = 0; cmds[i].name; i++) {
		if (streq(av[optind], cmds[i].name)) {
			ac -= optind;
			av += optind;
			getoptReset();
			return (cmds[i].fcn(ac, av));
		}
	}
	goto usage;
}