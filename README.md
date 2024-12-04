# Bonnie-64

A command-line program for Unix-flavored operating systems that attempts to characterize certain aspects of filesystem performance.

## Usage

```
Bonnie
    -d <directory-name> the directory in which Bonnie should write its test file. Default: .
    -r if set, causes Bonnie to randomize the contents of each buffer it writes. 
    -s the size of the test files, in MB (2**20).
    -m <machine-label> label for the output line.
    -f <txt|html> output file format.
    -u if set, prevents Bonnie from erasing its test file.
```
The purpose of the `-r` option is to defeat the effect of filesystems such as ZFS that avoid IO by spotting duplicate blocks in file and writing them only once.

## Author

Bonnie was originally written by Tim Bray in 1988 and revised several times since then. This version include valuable patches from Phost Feng.
