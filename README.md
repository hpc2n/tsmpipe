# tsmpipe

## About

tsmpipe is a utility to transfer file to/from an IBM Spectrum Protect (TSM)
server via a pipe to stdin/stdout.


## History

This tool is heavily inspired by the IBM `adsmpipe` utility, the author had
access to an `adsmpipe` AIX binary but wanted an implementation that worked
on other TSM-supported OS:es as well.

Development was motivated by IBM discontinuing the Andrew File System (AFS)
support in the TSM client. A replacement tool `afsbackup.pl` was developed,
and `tsmpipe` was implemented in order to have something to not being
dependant on the legacy `adsmpipe` binary.


## Building

`tsmpipe` has been known to work on AIX, Solaris and Linux. Recently only
used on Linux variants (mainly Ubuntu at HPC2N).

You need the TSM API package installed (`tivsm-api64` or `TIVsm-API64` on
Linux).

Then build using the appropriate Makefile:

`make -f Makefile.linux64`


## Usage

```
# tsmpipe -h
tsmpipe $Revision: 1.8 $, usage:
tsmpipe [-A|-B] [-c|-x|-d|-t] -s fsname -f filepath [-l len]
   -A and -B are mutually exclusive:
       -A  Use Archive objects
       -B  Use Backup objects
   -c, -x, -d and -t are mutually exclusive:
       -c  Create:  Read from stdin and store in TSM
       -x  eXtract: Recall from TSM and write to stdout
       -d  Delete:  Delete object from TSM
       -t  lisT:    Print filelist with filesizes to stdout
       -T  lisT:    Print filelist with volser ids to stdout
   -s and -f are required arguments:
       -s fsname   Name of filesystem in TSM
       -f filepath Path to file within filesystem in TSM
   -l length   Length of object to store. If guesstimating too large
               is better than too small
   -D desc     Description of archive object
   -O options  Extra options to pass to dsmInitEx
   -v          Verbose. More -v's gives more verbosity
```


## Other implemenations

* `adsmpipe` is the original IBM implementation
   * I was recently informed that it originates from an IBM RedBook. Notably https://github.com/jmkeyes/adsmpipe uses that source.
* https://dev.leenooks.net/deon/tsmpipe is a fork based on tsmpipe RCS version 1.6,
  approx https://github.com/hpc2n/tsmpipe/commit/116624fa8f8197d7c9ba41ddf5143642619150f5
