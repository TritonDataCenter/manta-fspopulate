/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2015, Joyent, Inc.
 */

/*
 * fspopulate: populates a directory with files roughly consistent with a Manta
 * storage dataset.
 */

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MIN(a, b) ((a < b) ? (a) : (b))

char *fsp_arg0;
const char *fsp_usagefmt = "usage: %s SIZE PATH\n";
char fsp_buf[10 * 1024 * 1024];

typedef struct {
	char		fsp_path[PATH_MAX];	/* root of tree to create */
	off_t		fsp_totsize;	/* total amount of bytes to write */
	int		fsp_nbulk;	/* number of "bulk" files to create */
	off_t		fsp_bulksize;	/* size of each "bulk" file */
	int		fsp_nsubdirs;	/* count of user directories */
	boolean_t	fsp_dryrun;	/* don't actually do anything */
} fspopulate_t;

static void usage(void);
static int fspopulate(fspopulate_t *);
static int populate_file(int, ssize_t);
static void init_buffer(char *, size_t);

int
main(int argc, char *argv[])
{
	int rv;
	char *endptr;
	unsigned long long totsz;
	fspopulate_t fspopconfig;

	fsp_arg0 = basename(argv[0]);
	
	if (argc < 3) {
		warnx("missing required arguments");
		usage();
	}

	/*
	 * Convert the "size" argument and allow common suffixes.
	 */
	totsz = strtoull(argv[1], &endptr, 0);
	if (strcasecmp(endptr, "t") == 0) {
		totsz *= 1024ULL * 1024 * 1024 * 1024;
	} else if (strcasecmp(endptr, "g") == 0) {
		totsz *= 1024ULL * 1024 * 1024;
	} else if (strcasecmp(endptr, "m") == 0) {
		totsz *= 1024 * 1024;
	} else if (strcasecmp(endptr, "k") == 0) {
		totsz *= 1024;
	} else if (*endptr != '\0') {
		warnx("unsupported size: \"%s\"", argv[1]);
		usage();
	}

	/*
	 * We currently hardcode the basic policy, which is that there will be
	 * "nbulk" files of size (total_size / 1024), making up 75% of the total
	 * data to write.  The rest will be made up of fixed-size files (usually
	 * smaller than the "large" files).  We'll put these files into
	 * "nsubdirs" different directories.
	 */
	bzero(&fspopconfig, sizeof (fspopconfig));
	(void) strlcpy(fspopconfig.fsp_path, argv[2],
	    sizeof (fspopconfig.fsp_path));
	fspopconfig.fsp_totsize = totsz;
	fspopconfig.fsp_nbulk = 768;
	fspopconfig.fsp_bulksize = totsz / 1024;
	fspopconfig.fsp_nsubdirs = 256;
	fspopconfig.fsp_dryrun = B_FALSE;

	init_buffer(fsp_buf, sizeof (fsp_buf));
	rv = fspopulate(&fspopconfig);
	return (rv == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

/*
 * Print a usage message and exit.
 */
static void
usage(void)
{
	(void) fprintf(stderr, fsp_usagefmt, fsp_arg0);
	exit(2);
}

/*
 * Initialize the static buffer we'll use to write data to files.
 */
static void
init_buffer(char *bufp, size_t bufsz)
{
	size_t i;

	/*
	 * The buffer contents are random in order to avoid being too
	 * compressible.  We don't strictly need it to be random.  In fact, we
	 * want this program to be deterministic, so we explicitly seed the
	 * random number generator.
	 */
	srand(1);
	for (i = 0; i < bufsz; i++) {
		bufp[i] = (char)rand();
	}
}

/*
 * Create a directory tree of files according to the parameters in "fsp".  This
 * is idempotent, and the set of files and their sizes is deterministic.  As a
 * result, we can quickly tell which work has already been done and avoid doing
 * it again.
 */
static int
fspopulate(fspopulate_t *fsp)
{
	int di, bi, fd, err;
	off_t totwritten, expectedsz;
	struct stat st;
	char pathname[PATH_MAX];

	(void) fprintf(stderr, "%-16s  %s\n", "path:", fsp->fsp_path);
	(void) fprintf(stderr, "%-16s  %-llu\n", "total bytes:",
	    (unsigned long long)fsp->fsp_totsize);
	(void) fprintf(stderr, "%-16s  %-u\n", "large files:", fsp->fsp_nbulk);
	(void) fprintf(stderr, "%-16s  %-llu bytes\n", "large file size:",
	    (unsigned long long)fsp->fsp_bulksize);
	(void) fprintf(stderr, "%-16s  %-u\n", "subdirs:", fsp->fsp_nsubdirs);

	if (fsp->fsp_dryrun) {
		return (0);
	}

	if (mkdirp(fsp->fsp_path, 0777) != 0 && errno != EEXIST) {
		warn("mkdirp \"%s\"", fsp->fsp_path);
		return (-1);
	}

	totwritten = 0;
	di = 0;
	bi = 0;
	while (totwritten < fsp->fsp_totsize) {
		if (bi < fsp->fsp_nsubdirs) {
			/*
			 * The first time through each directory, we have to
			 * create it first.
			 */
			(void) snprintf(pathname, sizeof (pathname),
			    "%s/dir%06d", fsp->fsp_path, di);
			if (mkdirp(pathname, 0777) == -1 && errno != EEXIST) {
				warn("mkdirp \"%s\"", fsp->fsp_path);
				return (-1);
			}
		}

		(void) snprintf(pathname, sizeof (pathname),
		    "%s/dir%06d/file%06d", fsp->fsp_path, di, bi);
		di = (di + 1) % fsp->fsp_nsubdirs;
		fd = open(pathname, O_APPEND | O_WRONLY | O_CREAT, 0666);
		if (fd < 0) {
			warn("open \"%s\"", pathname);
			return (-1);
		}

		if (fstat(fd, &st) != 0) {
			warn("fstat \"%s\"", pathname);
			(void) close(fd);
			return (-1);
		}

		expectedsz = bi < fsp->fsp_nbulk ?
		    fsp->fsp_bulksize : 10 * 1024 * 1024;
		expectedsz = MIN(expectedsz, fsp->fsp_totsize - totwritten);
		err = populate_file(fd, expectedsz - st.st_size);
		(void) close(fd);
		if (err != 0) {
			return (-1);
		}

		totwritten += expectedsz;
		if ((++bi) % 100 == 0) {
			(void) fprintf(stderr,
			    "completed %llu bytes after %d files\n"
			    "    (last: \"%s\" at %llu bytes)\n",
			    (unsigned long long)totwritten, bi, pathname,
			    (unsigned long long)expectedsz);
		}
	}

	return (0);
}

/*
 * Write "nbytes" bytes to "fd".  In this program "fd" is opened O_APPEND, so
 * the data will always wind up at the end.  We use a buffer initialized with
 * non-zero values to avoid ZFS compressing it too much, though the data itself
 * is pretty compressible.
 */
static int
populate_file(int fd, ssize_t nbytes)
{
	ssize_t ntowrite, nwritten, result;

	nwritten = 0;
	while (nwritten < nbytes) {
		ntowrite = MIN(nbytes - nwritten, ((ssize_t)sizeof (fsp_buf)));
		result = write(fd, fsp_buf, ntowrite);
		assert(result != 0);
		if (result == -1) {
			warn("write");
			return (-1);
		}

		nwritten += result;
	}

	return (0);
}
