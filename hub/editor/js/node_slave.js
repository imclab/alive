var fs 		= require('fs');
var path 	= require('path');
var io_in 	= require('socket.io').listen(8082);
var exec 	= require('child_process').exec;
var osc 	= require('./omgosc.js');

var MASTER_ADDRESS 	= "127.0.0.1";
var MASTER_OSC_PORT = 8010;
var OSC_IN 			= 8019;

var sender 		= new osc.UdpSender(MASTER_ADDRESS, MASTER_OSC_PORT);
var receiver 	= new osc.UdpReceiver(OSC_IN);

var master 		= null;
var currentDir 	= __dirname;

receiver.on('/print', function(e) {
	sender.send('/print', e.typetag, e.params);
});

receiver.on('/error', function(e) {
	console.log("SENDING ERROR");
	sender.send('/error', e.typetag, e.params);
});

io_in.sockets.on('connection', function (socket) {
	socket.addr = socket.handshake.address.address;
	socket.port = socket.handshake.address.port;

	master = socket;
	
	master.emit("handshake", { "data" : "Handshake received from " + socket.addr } );
	
	socket.on('pull', function(obj) {
		console.log("PULLING");
		exec("git pull origin master", {cwd: currentDir}, function() { console.log("MADE A PULL!"); } );
	});
	
	socket.on('cd', function(dir) {
		currentDir = path.resolve(currentDir, dir);
		console.log("CHANGED TO DIR", currentDir);
	});

	socket.on('disconnect', function () { 
		console.log("DISCONNECT : " + this.addr);
		if(master === this) {
			console.log("DISCONNECTING CLIENT");
			master = null;
		}
	});
});