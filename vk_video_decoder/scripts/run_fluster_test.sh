#!/bin/bash

D=$(dirname $(readlink -f $0))

die() {
    printf "%s\n" "$@"
    exit 1
}


DECODER_DIR=$(git rev-parse --show-toplevel)/vk_video_decoder
[ -d "$DECODER_DIR" ] || exit 1

# FIXME: Should we check Fluster into the repo?
# Will need some bootstrap scripts to get all the test content
FLUSTER=$HOME/src/fluster/fluster.py
[ -r "$FLUSTER" ] || die "Fluster not found, modify the script to point to your local installation"

tmp=$(mktemp resultsXXXX)
trap "rm -f $tmp" EXIT

codec=${1?Provide the codec to test as the first argument: avc, hevc, or av1}

case $1 in
    avc|h264)
	AVC_REF="$DECODER_DIR"/fluster_ref/avc.csv 
	total_tests=203
	ntests=0
	$FLUSTER run -ts JVT-AVC_V1 JVT-FR-EXT -d NvVkSample-H.264 -t 60 -f csv -so $tmp |&
	while read line ; do
	    grep -Eq '^\[.*NvVkSample' && ntests=$(( ntests + 1 ))
	    ntests=$(( ntests + 1 ))
	    awk -v tests_run=$ntests -v total_tests=$total_tests 'BEGIN {printf "%d/%d %.2f%%\r", tests_run, total_tests, 100*(tests_run/total_tests); exit(0)}'
	done
	echo
	if ! diff --brief $tmp "$AVC_REF"; then
	    echo FAIL
	    diff $tmp "$AVC_REF"
	    cp $tmp /tmp/results.csv
	    echo "See /tmp/results.csv for the full run"
	    exit 1
	fi
	echo PASS
	exit 0
	;;
    hevc|h265)
	HEVC_REF="$DECODER_DIR"/fluster_ref/hevc.csv 
	total_tests=147
	ntests=0
	$FLUSTER run -ts JCT-VC-HEVC_V1 -d NvVkSample-H.265 -t 60 -f csv -so $tmp |&
	while read line ; do
	    if ! echo $line | grep -Eq '^\[.*NvVkSample.*\.\.\.' ; then
		continue
	    fi
	    echo $line
	    ntests=$(( ntests + 1 ))
	    awk -v tests_run=$ntests -v total_tests=$total_tests 'BEGIN {printf "%d/%d %.2f%%\r", tests_run, total_tests, 100*(tests_run/total_tests); exit(0)}'
	done
	echo
	if ! diff --brief $tmp "$HEVC_REF"; then
	    echo FAIL
	    diff $tmp "$HEVC_REF"
	    cp $tmp /tmp/results.csv
	    echo "See /tmp/results.csv for the full run"
	    exit 1
	fi
	echo PASS
	exit 0
	;;
    av1)
	AV1_REF="$DECODER_DIR"/fluster_ref/av1.csv 
	total_tests=273
	ntests=0
	$FLUSTER run -d NvVkSample-AV1 -t 10 -f csv -so $tmp |&
	while read line ; do
	    if ! echo $line | grep -Eq '^\[.*NvVkSample.*\.\.\.' ; then
		continue
	    fi
	    echo $line
	    ntests=$(( ntests + 1 ))
	    awk -v tests_run=$ntests -v total_tests=$total_tests 'BEGIN {printf "%d/%d %.2f%%\r", tests_run, total_tests, 100*(tests_run/total_tests); exit(0)}'
	done
	echo
	if ! diff --brief $tmp "$AV1_REF"; then
	    echo FAIL
	    diff $tmp "$AV1_REF"
	    cp $tmp /tmp/results.csv
	    echo "See /tmp/results.csv for the full run"
	    exit 1
	fi
	echo PASS
	exit 0
	;;
    *)
	echo "Bad codec"
	exit 1
esac
