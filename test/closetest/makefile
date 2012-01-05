closetest:	closetest.cpp
		g++ closetest.cpp -o closetest


# to be used on linux.
# watch fds for connection being opened and closed.
test:		closetest
		-../../graphserv -g ../../example-gsgroups.conf -p ../../example-gspasswd.conf -c ../../graphcore/graphcore & echo $$! > PID
		sleep 0.5
		(echo "authorize password fred:test"; echo "create-graph test") | nc localhost 6666
		xterm -e watch -n 1 lsof -p $$(pidof graphserv) & sleep 1
		# spawn lots of instances 
		for i in $$(seq 1 5000); do ./closetest 127.0.0.1 & done
		sleep 10
		kill $$(cat PID)
		rm PID
