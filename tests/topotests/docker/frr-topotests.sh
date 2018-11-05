#!/bin/bash
#
# Copyright 2018 Network Device Education Foundation, Inc. ("NetDEF")
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be
# included in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
# BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
# ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
# CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

set -e

if [[ "$1" = "-h" ]] || [[ "$1" = "--help" ]]; then
	cat >&2 <<-EOF

	This script runs the FRRouting topotests on the FRR tree
	in the current working directory.

	Usage: $0 [args...]

	Its behavior can be modified by the following environment variables:

	TOPOTEST_AUTOLOAD       If set to 1, the script will try to load necessary
	                        kernel modules without asking for confirmation first.

	TOPOTEST_BUILDCACHE     Docker volume used for caching multiple FRR builds
	                        over container runs. By default a
	                        \`topotest-buildcache\` volume will be created for
	                        that purpose.

	TOPOTEST_CLEAN          Clean all previous build artifacts prior to
	                        building. Disabled by default, set to 1 to enable.

	TOPOTEST_DOC            Build the documentation associated with FRR.
	                        Disabled by default, set to 1 to enable.

	TOPOTEST_FRR            If set, don't test the FRR in the current working
	                        directory, but the one at the given path.

	TOPOTEST_LOGS           If set, don't use \`/tmp/topotest_logs\` directory
	                        but use the provided path instead.

	TOPOTEST_OPTIONS        These options are appended to the docker-run
	                        command for starting the tests.

	TOPOTEST_PATH           If set, don't use the tests built into the image
	                        but the ones at the given path.

	TOPOTEST_SANITIZER      Controls whether to use the address sanitizer.
	                        Enabled by default, set to 0 to disable.

	TOPOTEST_VERBOSE        Show detailed build output.
	                        Enabled by default, set to 0 to disable.

	To get information about the commands available inside of the container,
	run \`$0 help\`.
	EOF
	exit 1
fi

#
# These two modules are needed to run the MPLS tests.
# They are often not automatically loaded.
#
# We cannot load them from the container since we don't
# have host kernel modules available there. If we load
# them from the host however, they can be used just fine.
#

for module in mpls-router mpls-iptunnel; do
	if modprobe -n $module 2> /dev/null; then
		:
	else
		# If the module doesn't exist, we cannot do anything about it
		continue
	fi

	if [ $(grep -c ${module/-/_} /proc/modules) -ne 0 ]; then
		# If the module is loaded, we don't have to do anything
		continue
	fi

	if [ "$TOPOTEST_AUTOLOAD" != "1" ]; then
		echo "To run all the possible tests, we need to load $module."
		echo -n "Do you want to proceed? [y/n] "
		read answer
		if [ x"$answer" != x"y" ]; then
			echo "Not loading."
			continue
		fi
	fi

	if [ x"$(whoami)" = x"root" ]; then
		modprobe $module
	else
		sudo modprobe $module
	fi
done

if [ -z "$TOPOTEST_LOGS" ]; then
	mkdir -p /tmp/topotest_logs
	TOPOTEST_LOGS="/tmp/topotest_logs"
fi

if [ -z "$TOPOTEST_FRR" ]; then
	TOPOTEST_FRR="$(git rev-parse --show-toplevel || true)"
	if [ -z "$TOPOTEST_FRR" ]; then
		echo "Could not determine base of FRR tree." >&2
		echo "frr-topotests only works if you have your tree in git." >&2
		exit 1
	fi
fi

if [ -z "$TOPOTEST_BUILDCACHE" ]; then
	TOPOTEST_BUILDCACHE=topotest-buildcache
	docker volume inspect "${TOPOTEST_BUILDCACHE}" &> /dev/null \
		|| docker volume create "${TOPOTEST_BUILDCACHE}"
fi

set -- --rm -ti \
	-v "$TOPOTEST_LOGS:/tmp" \
	-v "$TOPOTEST_FRR:/root/host-frr:ro" \
	-v "$TOPOTEST_BUILDCACHE:/root/persist" \
	-e "TOPOTEST_CLEAN=$TOPOTEST_CLEAN" \
	-e "TOPOTEST_VERBOSE=$TOPOTEST_VERBOSE" \
	-e "TOPOTEST_DOC=$TOPOTEST_DOC" \
	-e "TOPOTEST_SANITIZER=$TOPOTEST_SANITIZER" \
	--privileged \
	$TOPOTEST_OPTIONS \
	frrouting/topotests "$@"

if [ -n "TOPOTEST_PATH" ]; then
	set -- -v "$TOPOTEST_PATH:/root/topotests:ro" "$@"
fi

exec docker run "$@"