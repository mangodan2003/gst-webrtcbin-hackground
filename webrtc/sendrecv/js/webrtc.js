/* vim: set sts=4 sw=4 et :
 *
 * Demo Javascript app for negotiating and streaming a sendrecv webrtc stream
 * with a GStreamer app.
 * 
 * Author: Dan Squires <mangodan2003@gmail.com>, Nirbheek Chauhan <nirbheek@centricular.com>
 */

// Set this to override the automatic detection in websocketServerConnect()
var ws_server;
var ws_port;
// Set this to use a specific peer id instead of a random one
var default_peer_id = "test";
// Override with your own STUN servers if you want
var rtc_configuration = {iceServers: [{urls: "stun:stun.services.mozilla.com"},
                                      {urls: "stun:stun.l.google.com:19302"}]};
// The default constraints that will be attempted. Can be overriden by the user.
var default_constraints = {video: true, audio: true};

var connect_attempts = 0;
var peer_connection;
let makingOffer = false, ignoreOffer = false;
let polite = true;
let pong = 0;
var send_channel;
var ws_conn;
// Promise for local stream after constraints are approved by the user
var local_stream_promise;

function send(json) {
    ws_conn.send(JSON.stringify(json));
}

function setButtonState(button, value) {
    document.getElementById(button).value = value;
}

function setConnectButtonState(state) {
    setButtonState("peer-connect-button", state);
}

function setSendVideoButtonState(state) {
    setButtonState("send-video-button", state);
}

function setRecvVideoButtonState(state) {
    setButtonState("recv-video-button", state);
}

function setRecvAudioButtonState(state) {
    setButtonState("recv-audio-button", state);
}

function setSendAudioButtonState(state) {
    setButtonState("send-audio-button", state);
}

function wantRemoteOfferer() {
   return document.getElementById("remote-offerer").checked;
}

function onConnectClicked() {
    if (document.getElementById("peer-connect-button").value == "Disconnect") {
        resetState();
        return;
    }

    var id = document.getElementById("peer-connect").value;
    if (id == "") {
        alert("Peer id must be filled out");
        return;
    }

    ws_conn.send("SESSION " + id);
    setButtonState("peer-connect-button", "Disconnect");
}

function onSendVideoClicked() {
    if (document.getElementById("send-video-button").value == "Send Video") {
        console.log('Send Video clicked.')
        setSendVideoButtonState("Stop Sending Video");

        local_stream_promise = getLocalMediaStream({video: true, audio: false}).then((stream) => {
            console.log('Adding local video');
            for (const track of stream.getTracks()) {
                peer_connection.addTrack(track);
                console.log('Added track:', track);
            }
        }).catch(setError);
        return;
    } else
    if (document.getElementById("send-video-button").value == "Stop Sending Video") {
        console.log('Stop Sending Video clicked.')
        setSendVideoButtonState("Send Video");
        return;
    }
}

function onSendAudioClicked() {
    if (document.getElementById("send-audio-button").value == "Send Audio") {
        console.log('Send Audio clicked.')
        setSendAudioButtonState("Stop Sending Audio");

        local_stream_promise = getLocalMediaStream({video: false, audio: true}).then((stream) => {
            console.log('Adding local audio');
            for (const track of stream.getTracks()) {
                peer_connection.addTrack(track);
                console.log('Added track:', track);
            }
        }).catch(setError);
        return;
    } else
    if (document.getElementById("send-audio-button").value == "Stop Sending Audio") {
        console.log('Stop Sending Audio clicked.')
        setSendAudioButtonState("Send Audio");
        return;
    }
}

function onRecvVideoClicked() {
    console.log("onRecvVideoClicked()");
    if (document.getElementById("recv-video-button").value == "Receive Video") {
        console.log('Recv Video clicked.')
        setRecvVideoButtonState("Stop Receiving Video");

        send_channel.send("RECV VIDEO START");
    } else
    if (document.getElementById("recv-video-button").value == "Stop Receiving Video") {
        console.log('Stop Receiving Video clicked.')
        setRecvVideoButtonState("Receive Video");
        send_channel.send("RECV VIDEO STOP");
        return;
    }
}

function onRecvAudioClicked() {
    console.log("onRecvAudioClicked()");
    if (document.getElementById("recv-audio-button").value == "Receive Audio") {
        console.log('Recv Audio clicked.')
        setRecvAudioButtonState("Stop Receiving Audio");

        send_channel.send("RECV AUDIO START");
    } else
    if (document.getElementById("recv-audio-button").value == "Stop Receiving Audio") {
        console.log('Stop Receiving Audio clicked.')
        setRecvAudioButtonState("Receive Audio");
        send_channel.send("RECV AUDIO STOP");
        return;
    }
}


function getOurId() {
    return Math.floor(Math.random() * (9000 - 10) + 10).toString();
}

function resetState() {
    // This will call onServerClose()
    ws_conn.close();
}

function handleIncomingError(error) {
    setError("ERROR: " + error);
    resetState();
}

function getVideoElement() {
    return document.getElementById("stream");
}

function setStatus(text) {
    console.log(text);
    var span = document.getElementById("status")
    // Don't set the status if it already contains an error
    if (!span.classList.contains('error'))
        span.textContent = text;
}

function setError(text) {
    console.error(text);
    var span = document.getElementById("status")
    span.textContent = text;
    span.classList.add('error');
}

function resetVideo() {
    // Release the webcam and mic
    if (local_stream_promise)
        local_stream_promise.then(stream => {
            if (stream) {
                stream.getTracks().forEach(function (track) { track.stop(); });
            }
        });

    // Reset the video element and stop showing the last received frame
    var videoElement = getVideoElement();
    videoElement.pause();
    videoElement.src = "";
    videoElement.load();
}

// SDP offer received from peer, set remote description and create an answer
async function onIncomingSDP(sdp) {
    const offerCollision = sdp.type == "offer" &&
                            (makingOffer || peer_connection.signalingState != "stable");

    ignoreOffer = !polite && offerCollision;
    if (ignoreOffer) {
        return;
    }
    if (offerCollision) {
    await Promise.all([
        peer_connection.setLocalDescription({type: "rollback"}),
        peer_connection.setRemoteDescription(sdp)
    ]);
    } else {
        await peer_connection.setRemoteDescription(sdp);
    }
    if (sdp.type == "offer") {
        await peer_connection.setLocalDescription(await peer_connection.createAnswer());
        send({sdp: peer_connection.localDescription});
    }
}

// Local description was set, send it to peer
function onLocalDescription(desc) {
    console.log("Got local description: " + JSON.stringify(desc));
    peer_connection.setLocalDescription(desc).then(function() {
        setStatus("Sending SDP " + desc.type);
        sdp = {'sdp': peer_connection.localDescription}
        ws_conn.send(JSON.stringify(sdp));
    });
}

function generateOffer() {
    peer_connection.createOffer().then(onLocalDescription).catch(setError);
}

// ICE candidate received from peer, add it to the peer connection
function onIncomingICE(ice) {
    var candidate = new RTCIceCandidate(ice);
    peer_connection.addIceCandidate(candidate).catch(setError);
}

function onServerMessage(event) {
    console.log("Received " + event.data);
    switch (event.data) {
        case "HELLO":
            setStatus("Registered with server, waiting for call");
            return;

        case "SESSION_OK":
            setStatus("Starting negotiation");
            ws_conn.send("OFFER_REQUEST");
            setStatus("Sent OFFER_REQUEST, waiting for offer");
            return;

            if (!peer_connection)
                createCall(null);

            return;

        case "OFFER_REQUEST":
            // The peer wants us to set up and then send an offer
            if (!peer_connection)
                createCall(null);
            //generateOffer();
            return;
        default:
            if (event.data.startsWith("ERROR")) {
                handleIncomingError(event.data);
                return;
            }
            // Handle incoming JSON SDP and ICE messages
            try {
                msg = JSON.parse(event.data);
            } catch (e) {
                if (e instanceof SyntaxError) {
                    handleIncomingError("Error parsing incoming JSON: " + event.data);
                } else {
                    handleIncomingError("Unknown error parsing response: " + event.data);
                }
                return;
            }

            // Incoming JSON signals the beginning of a call
            if (!peer_connection)
                createCall(msg);

            if (msg.sdp != null) {
                onIncomingSDP(msg.sdp);
            } else if (msg.ice != null) {
                onIncomingICE(msg.ice);
            } else {
                handleIncomingError("Unknown incoming JSON: " + msg);
            }
    }
}

function onServerClose(event) {
    setStatus('Disconnected from server');
    resetVideo();

    if (peer_connection) {
        peer_connection.close();
        peer_connection = null;
    }

    // Reset after a second
    window.setTimeout(websocketServerConnect, 1000);
}

function onServerError(event) {
    setError("Unable to connect to server, did you add an exception for the certificate?")
    // Retry after 3 seconds
    window.setTimeout(websocketServerConnect, 3000);
}

function getLocalMediaStream(constraints) {

    // Add local stream
    if (navigator.mediaDevices.getUserMedia) {
        return navigator.mediaDevices.getUserMedia(constraints);
    } else {
        errorUserMediaHandler();
    }
}

function websocketServerConnect() {
    connect_attempts++;
    if (connect_attempts > 3) {
        setError("Too many connection attempts, aborting. Refresh page to try again");
        return;
    }
    // Clear errors in the status span
    var span = document.getElementById("status");
    span.classList.remove('error');
    span.textContent = '';

    // Fetch the peer id to use
    peer_id = default_peer_id || getOurId();
    ws_port = ws_port || '8443';
    if (window.location.protocol.startsWith ("file")) {
        ws_server = ws_server || "127.0.0.1";
    } else if (window.location.protocol.startsWith ("http")) {
        ws_server = ws_server || window.location.hostname;
    } else {
        throw new Error ("Don't know how to connect to the signalling server with uri" + window.location);
    }
    var ws_url = 'wss://' + ws_server + ':' + ws_port
    setStatus("Connecting to server " + ws_url);
    ws_conn = new WebSocket(ws_url);
    /* When connected, immediately register with the server */
    ws_conn.addEventListener('open', (event) => {
        document.getElementById("peer-id").textContent = peer_id;
        ws_conn.send('HELLO ' + peer_id);
        setStatus("Registering with server");
        setConnectButtonState("Connect");
    });
    ws_conn.addEventListener('error', onServerError);
    ws_conn.addEventListener('message', onServerMessage);
    ws_conn.addEventListener('close', onServerClose);
}

function onRemoteTrack(event) {
    console.log("onRemoteTrack:", event);
    let stream = event.streams[0];
    if (getVideoElement().srcObject !== stream) {
        console.log('Incoming stream');
        getVideoElement().srcObject = stream;

        stream.onremovetrack = ({track}) => {
          console.log(`${track.kind} track was removed.`);
          if (!stream.getTracks().length) {
            console.log(`stream ${stream.id} emptied (effectively removed).`);
            getVideoElement().srcObject = null;
          }
        }
    }
}

function errorUserMediaHandler() {
    setError("Browser doesn't support getUserMedia!");
}

const handleDataChannelOpen = (event) =>{
    console.log("dataChannel.OnOpen", event);
};

const handleDataChannelMessageReceived = (event) =>{
    //setStatus("Received data channel message");
    if (typeof event.data === 'string' || event.data instanceof String) {
        console.log('Incoming string message: ' + event.data);
        textarea = document.getElementById("text")
        textarea.value = textarea.value + '\n' + event.data
        send_channel.send("PONG " + pong.toString());
        pong++;
    }
//    } else {
//        console.log('Incoming data message');
//    }
};

const handleDataChannelError = (error) =>{
    console.log("dataChannel.OnError:", error);
};

const handleDataChannelClose = (event) =>{
    console.log("dataChannel.OnClose", event);
    textarea.value = "";
    pong = 0;
};

function onDataChannel(event) {
    setStatus("Data channel created");
    let receiveChannel = event.channel;
    receiveChannel.onopen = handleDataChannelOpen;
    receiveChannel.onmessage = handleDataChannelMessageReceived;
    receiveChannel.onerror = handleDataChannelError;
    receiveChannel.onclose = handleDataChannelClose;
}

function createCall(msg) {
    // Reset connection attempts because we connected successfully
    connect_attempts = 0;

    console.log('Creating RTCPeerConnection');

    peer_connection = new RTCPeerConnection(rtc_configuration);
    send_channel = peer_connection.createDataChannel('label', null);
    send_channel.onopen = handleDataChannelOpen;
    send_channel.onmessage = handleDataChannelMessageReceived;
    send_channel.onerror = handleDataChannelError;
    send_channel.onclose = handleDataChannelClose;
    peer_connection.ondatachannel = onDataChannel;
    peer_connection.ontrack = onRemoteTrack;
    peer_connection.onnegotiationneeded = async () => {
      console.log("onnegotiationneeded");
//        ws_conn.send("OFFER_REQUEST");
      try {
        makingOffer = true;
        const offer = await peer_connection.createOffer();
        if (peer_connection.signalingState != "stable") return;
        await peer_connection.setLocalDescription(offer);
        send({sdp: peer_connection.localDescription});
      } catch (e) {
        log(`ONN ${e}`);
      } finally {
        makingOffer = false;
      }
    };
    /* Send our video/audio to the other peer */

    if (msg != null && !msg.sdp) {
        console.log("WARNING: First message wasn't an SDP message!?");
    }

    peer_connection.onicecandidate = (event) => {
        // We have a candidate, send it to the remote party with the
        // same uuid
        if (event.candidate == null) {
                console.log("ICE Candidate was null, done");
                return;
        }
        ws_conn.send(JSON.stringify({'ice': event.candidate}));
    };

    if (msg != null)
        setStatus("Created peer connection for call, waiting for SDP");

    setConnectButtonState("Disconnect");
    return local_stream_promise;
}
