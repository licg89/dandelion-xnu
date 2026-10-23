#!/bin/sh
# Minimal stand-in for Apple's availability.pl.
#
# XNU's bsd/sys/make_symbol_aliasing.sh runs availability.pl to enumerate the
# platform versions it should emit __DARWIN_ALIAS_STARTING_* macros for.  The
# iOS SDK that ships with Xcode does not contain that script (only internal
# Apple SDKs do), and XNU only needs the version list, not Apple's database.
#
# Set XNU_AVAILABILITY_PL to this file when cross building outside Apple.
#
# usage: availability.pl --ios | --macosx

set -eu

case "${1:-}" in
--ios)
	for major in 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17; do
		echo "$major.0"
	done
	echo 3.1; echo 3.2
	echo 4.1; echo 4.2; echo 4.3
	echo 5.1
	echo 6.1
	echo 7.1
	echo 8.1; echo 8.2; echo 8.3; echo 8.4
	echo 9.1; echo 9.2; echo 9.3
	echo 10.1; echo 10.2; echo 10.3
	echo 11.1; echo 11.2; echo 11.3; echo 11.4
	echo 12.1; echo 12.2; echo 12.3; echo 12.4
	echo 13.1; echo 13.2; echo 13.3; echo 13.4; echo 13.5; echo 13.6; echo 13.7
	echo 14.1; echo 14.2; echo 14.3; echo 14.4; echo 14.5; echo 14.6; echo 14.7; echo 14.8
	echo 15.1; echo 15.2; echo 15.3; echo 15.4; echo 15.5; echo 15.6; echo 15.7; echo 15.8
	echo 16.1; echo 16.2; echo 16.3; echo 16.4; echo 16.5; echo 16.6; echo 16.7; echo 16.8
	echo 17.1; echo 17.2; echo 17.3; echo 17.4; echo 17.5
	;;
--macosx)
	# 10.x uses the legacy decimal encoding, 11+ the flat one.
	for minor in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
		echo "10.$minor"
	done
	for major in 11 12 13 14; do
		last=7
		[ "$major" = 14 ] && last=5
		i=0
		while [ "$i" -le "$last" ]; do
			echo "$major.$i"
			i=$((i + 1))
		done
	done
	;;
*)
	echo "usage: $0 --ios | --macosx" >&2
	exit 1
	;;
esac
