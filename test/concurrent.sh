#!/bin/bash

CONCURRENCY=8
ARCSPERPART=20000

# server binary
SERVBIN="../graphserv.dbg"
# core binary. must use the debug version for list-by-* commands.
COREBIN="../graphcore/graphcore.dbg"
# use the example password and group files
PWFILE="../example-gspasswd.conf"
GRPFILE="../example-gsgroups.conf"
# tcp port the server will listen on
TCPPORT=6666

NUMARCS=$(( $ARCSPERPART * $CONCURRENCY ))

# build core & server
echo "building..."
(make -C.. && make -C../graphcore) >/dev/null
if ! [[ -x $SERVBIN ]] || ! [[ -x $COREBIN ]] ; then echo 'Build failed.'; exit 1; fi

if true; then 

echo "creating $NUMARCS random arcs..."

#rm tmp-arcs* result-arcs*

# create some random arcs
[ -f tmp-arcs ] && rm tmp-arcs
i=0
until [[ $i == $NUMARCS ]] ; do 
	# $RANDOM is 0..32767. multiply to generate something between 0..ffffffff (not uniformly distributed)
	echo $(( $RANDOM * $RANDOM % $NUMARCS + 1 ))", "$(( $RANDOM * $RANDOM % $NUMARCS + 1 )) >> tmp-arcs
	let i++
done

# sort them using graphcore
(echo 'add-arcs < tmp-arcs'; echo 'list-by-tail 0') | $COREBIN > tmp-arcs-sorted || exit 1
# | grep -v "OK." | egrep -v "^$" 

# break them into pieces
i=0
part=0
until [[ $i == $NUMARCS ]] ; do
	head -n $(( $i + $ARCSPERPART )) tmp-arcs | tail -n $ARCSPERPART > tmp-arcs-part$part
	let i+=$ARCSPERPART
	let part++
done

fi

# start the server
$SERVBIN -lia -p $TCPPORT -c $COREBIN -p $PWFILE -g $GRPFILE & SERVPID=$! 

sleep 1

ps -p $SERVPID >/dev/null || exit 1

# create test graph
RESULT=$(
	(	echo 'authorize password fred:test'
		echo 'create-graph test'
		sleep 1
	) | nc localhost $TCPPORT )

if ! [[ $RESULT =~ OK.*OK.* ]] ; then echo "couldn't create graph. output:"; echo "$RESULT"; kill $SERVPID; exit 1; fi


# fill graph with the generated random data, concurrently, using several sessions
PIDLIST=""
part=0
until [[ $part == $CONCURRENCY ]] ; do
	echo "starting part $part."
	>f$part
	(
		(echo 'authorize password fred:test'; 
		echo 'use-graph test'; 
		echo 'add-arcs:'; cat tmp-arcs-part$part; echo ""; 
		# make the process writing to netcat wait for the last "OK." output coming from netcat.
		# if we don't do this, netcat will finish early.
		while true; do if egrep "^OK\. $" f$part >/dev/null; then exit 0; fi; sleep 0.2; done
		sleep 1) \
		| nc localhost $TCPPORT | tee f$part
	) & PIDLIST="$PIDLIST $!"

	let part++
done

# wait for all sessions to finish
wait $PIDLIST

(echo "use-graph test";
echo "list-by-tail 0";
# wait for empty line.
while true; do sleep 0.1; if tail -n 1 result-set | egrep "^$" >/dev/null; then exit 0; fi; done) |
	nc localhost $TCPPORT > result-set

grep -v "OK." result-set | egrep -v "^$" > result-arcs

echo "comparing result files with diff..."

# compare result files

if ! diff tmp-arcs-sorted result-arcs >/dev/null ; then 
	echo "Test FAILED! Result files don't match."; exit 1; 
fi

echo "Test SUCCEEDED. Result files match."

rm tmp-arcs* result-arcs*

kill $SERVPID

exit 0

