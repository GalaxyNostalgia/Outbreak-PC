#!/bin/sh
# resume_unpack.sh DIR: unpack DIR/resume.tar.gz.part* back where resume_pack.sh took them from
# (the same absolute paths: the repos in /home/claude, WORK in ~/outbreak-work).
cat "$1"/resume.tar.gz.part* | gunzip | tar -C / -xSf -
