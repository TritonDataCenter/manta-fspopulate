# fspopulate

"fspopulate" is a small C program to create a two-level directory tree of files.
These files' sizes sum up to a desired total dataset size (like "1TB").  The
files are put into 256 top-level directories.

To try to make a reasonable mix of file sizes, 3/4 of the total data is
represented by files of size 1/1024 of the total dataset size.  The rest of the
files are 10MB.  The last file may be smaller than 10MB.  For example, if you
ask for 1TB, you'll get 768 1GB files, plus 26214 10MB files, plus one file
that's 4MB.

The data written is not very compressible.  (Empirically, "gzip" with default
settings produces files that are larger than the original file.)

The program is idempotent and deterministic.  Before writing out each file, it
checks whether it's already the correct size, and if so, leaves it alone.  As a
result, when running it on an existing dataset, it only writes out what's
necessary to fill up the dataset.  It never removes any data.
