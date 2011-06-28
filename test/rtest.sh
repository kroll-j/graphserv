#!/bin/bash

# server binary
SERVBIN="../graphserv.dbg"
# core binary. must use the debug version for list-by-* commands.
COREBIN="../graphcore/graphcore.dbg"
# use the example password and group files
PWFILE="../example-gspasswd.conf"
GRPFILE="../example-gsgroups.conf"
# tcp port the server will listen on
TCPPORT=6666

HOST=nightshade.toolserver.org


function random() {
	echo 'r = ' $RANDOM ' % 10000; scale=8; r * 0.0001 * ' $1 + $2 | bc
}

function add_arcs() {
	while true; do (
		echo 'authorize password fred:test'
	  	echo 'use-graph test'
		local N=$(( $RANDOM % 5000 + 1 ))
		( echo 'add-arcs:';
		  for k in $(seq 1 $N ) ; do 
			echo "$(( $RANDOM % 1000 + 1)), $(( $RANDOM % 1000 + 1))";
		  done; echo "" ) ) | nc $HOST $TCPPORT >/dev/null || exit 0
		sleep $(random 1 .01)
	done
}

function remove_arcs() {
	while true; do
		N=$( (echo 'use-graph test'; echo 'stats'; sleep 2) | nc $HOST $TCPPORT | grep ArcCount | cut -d ',' -f 2 )
		if [[ x$N == x ]]; then N=100; fi
		if (( $N > 50000 )); then 
			N=$(( $N / 2 ))
			echo '		***' removing $N
		 	(echo 'authorize password fred:test'; echo 'use-graph test'; 
			 echo "list-by-head 0 $N > tmpout") | nc $HOST $TCPPORT
			(echo 'authorize password fred:test'; 
			 echo 'use-graph test'
			 echo 'remove-arcs < tmpout'
			 echo stats
			 sleep 10
			) | nc $HOST $TCPPORT || exit 0
			#date +'%F %H:%M:%S'
			#echo '====== server: ======'
			#pmap $SERVPID | grep total
			#echo '====== core: ======'
			#pmap $COREPID | grep total
		fi
		sleep .5	#$(random .1 .1)
	done
}


function intcleanup() {
	echo ' SIGINT received, terminating server.'
	kill $SERVPID
	exit 0
}


if [[ $HOST == localhost ]]; then
# start the server
$SERVBIN -lia -p $TCPPORT -c $COREBIN -p $PWFILE -g $GRPFILE > graphserv.log 2>&1 & SERVPID=$! 

sleep .5

# check if the server failed to start up.
ps -p $SERVPID >/dev/null || exit 1

COREPID=$( (echo 'authorize password fred:test'; echo 'create-graph test'; sleep 1) | nc localhost $TCPPORT | grep "spawned pid" | sed 's/^.*pid \([0-9]*\).*/\1/')

echo core pid: $COREPID

else

(echo 'authorize password fred:test'; echo 'create-graph test'; sleep 1) | nc $HOST $TCPPORT

fi

trap intcleanup SIGINT

add_arcs &
remove_arcs &

(while true; do sleep 10; done)


