/* Copyright (c) 2009, Mamta Singh. See README for details. */
package Phone
{
	import flash.events.AsyncErrorEvent;
	import flash.events.ErrorEvent;
	import flash.events.EventDispatcher;
	import flash.events.IOErrorEvent;
	import flash.events.NetStatusEvent;
	import flash.events.SecurityErrorEvent;
	import flash.net.NetConnection;
	import flash.net.NetStream;
	import flash.net.ObjectEncoding;
	import flash.net.SharedObject;
	import flash.net.Responder;
	import mx.collections.ArrayCollection;

	import flash.events.Event;
	import flash.events.TimerEvent;
	import flash.utils.Timer;
	import flash.media.Microphone;
	import flash.system.Security;
	import flash.system.SecurityPanel;
	import flash.system.Capabilities;

	import Phone.PhoneEvent;

	[Event(name="OnInvited", type="PhoneEvent.OnInvited")]
	[Event(name="OnCancelled", type="PhoneEvent.OnCancelled")]
	[Event(name="OnAccepted", type="PhoneEvent.OnAccepted")]
	[Event(name="OnRejected", type="PhoneEvent.OnRejected")]
	[Event(name="OnByed", type="PhoneEvent.OnByed")]
	[Event(name="OnConnected", type = "PhoneEvent.OnConnected")]
	[Event(name="OnError", type="PhoneEvent.OnError")]
	[Event(name="OnHold", type="PhoneEvent.OnHold")]
	[Event(name="OnUnhold", type="PhoneEvent.OnUnhold")]
	[Event(name="OnRinging", type="PhoneEvent.OnRinging")]


	public class Connector extends EventDispatcher
	{
		//--------------------------------------
		// CLASS CONSTANTS
		//--------------------------------------
		public static const IDLE:String      = "idle";
		public static const CONNECTING:String= "connecting";
		public static const CONNECTED:String = "connected";
		public static const OUTBOUND:String  = "outbound";
		public static const INBOUND:String   = "inbound";
		public static const ACTIVE:String    = "active";

		//--------------------------------------
		// PRIVATE PROPERTIES
		//--------------------------------------

		private var _currentState : String        = IDLE;
		private var _netConnection: NetConnection = null;
		private var _playStream   : NetStream     = null;
		private var _publishStream: NetStream     = null;

		//--------------------------------------
		// PUBLIC PROPERTIES
		//--------------------------------------

		[Bindable]
		public var gatewayURL:String;
		[Bindable]
		public var authName:String;
		[Bindable]
		public var authPass:String;
		[Bindable]
		public var displayName:String;
		[Bindable]
		public var contactAddress:String;
		[Bindable]
		public var selectedAudio:String = null;
		[Bindable]
		public var selectedVideo:String = null;

		//--------------------------------------
		// CONSTRUCTOR
		//--------------------------------------

		public function Connector()
		{
		}

		//--------------------------------------
		// GETTERS/SETTERS
		//--------------------------------------

		[Bindable]
		public function get currentState(): String
		{
			return _currentState;
		}

		public function set currentState(value: String): void
		{
			var oldValue:String = _currentState;
			_currentState = value;

			switch (value) {
				case IDLE:
				case CONNECTED:
					stopPublishPlay();
					break;
				case ACTIVE:
					startPublishPlay();
					break;
				case CONNECTING:
				case OUTBOUND:
				case INBOUND:
					break;
			}
			_trace("State changed from {0} to {1}", oldValue, _currentState);
		}

		//--------------------------------------
		// PUBLIC METHODS
		//--------------------------------------

		public function connect(host:String, port:String, user:String, password:String, displayName:String):void
		{
			if (currentState != IDLE) {
				_trace("Tried to connect but no IDLE");
				return;
			}

			currentState = CONNECTING;
			this.gatewayURL = _("rtmp://{0}:{1}/{2}", host, port, "asterisk");
			this.authName = user;
			this.authPass = password;
			this.displayName = displayName;

			if (_netConnection != null) {
				_netConnection.close();
				_netConnection = null;
				_playStream = _publishStream = null;
			}

			_netConnection = new NetConnection();
			_netConnection.client = this;
			_netConnection.addEventListener(NetStatusEvent.NET_STATUS, netStatusHandler, false, 0, true);
			_netConnection.addEventListener(IOErrorEvent.IO_ERROR, errorHandler, false, 0, true);
			_netConnection.addEventListener(SecurityErrorEvent.SECURITY_ERROR, errorHandler, false, 0, true);
			_netConnection.addEventListener(AsyncErrorEvent.ASYNC_ERROR, errorHandler, false, 0, true);

			_netConnection.connect(this.gatewayURL, this.authName, this.authPass, this.displayName);
		}

		public function disconnect(): void
		{
			currentState = IDLE;
			if (_netConnection != null) {
				_netConnection.close();
				_netConnection = null;
				_playStream = _publishStream = null;
			}
		}

		/* RPC commands we send to the server */

		/**
		 * The method initiates outbound call to the given destination number.
		 * It invokes the "invite" RPC on the connection.
		 */
		public function invite(number: String): void
		{
			_trace("invite: " + number);
			contactAddress = number;
			if (currentState == CONNECTED) {
				currentState = OUTBOUND;
				_netConnection.call("invite", null, contactAddress);
			} else {
				_trace("Cannot invite() while state is " + currentState);
			}
		}

		/**
		 * Accepts a pending incoming call.
		 * It invokes the "accept" RPC on the connection.
		 */
		public function accept(): void
		{
			_trace("accept");
			if (currentState == INBOUND) {
				_netConnection.call("accept", null);
			} else {
				_trace("Cannot accept() while state is " + currentState);
			}
		}

		/**
		 * Rejects a pending incoming call.
		 * It invokes the "reject" RPC on the connection.
		 */
		public function reject(reason: String): void
		{
			_trace("reject: " + reason);
			if (currentState == INBOUND) {
				_netConnection.call("reject", null, reason);
				currentState = CONNECTED;
			} else {
				_trace("Cannot reject() while state is " + currentState);
			}
		}

		/**
		 * The method terminates an active call or cancels an outbound call.
		 * It invokes the "bye" RPC on the connection.
		 */
		public function bye(): void
		{
			_trace("bye");
			if (currentState == OUTBOUND || currentState == ACTIVE) {
				_netConnection.call("hangup", null);
				currentState = CONNECTED;
			} else {
				_trace("Cannot bye() while state is " + currentState);
			}
		}

		/**
		 * The method sends a DTMF digit to the remote party in an active call.
		 * It invokes the "sendDTMF" RPC on the connection.
		 */
		public function sendDigit(digit: String): void
		{
			_trace("sending digit " + digit);
			if (currentState == ACTIVE) {
				_netConnection.call("sendDTMF", null, digit);
			} else {
				_trace("Cannot sendDigit() while state is " + currentState);
			}
		}

		/**
		 * The method put the remote party on hold or off hold
		 * It invokes the "hold" RPC on the connection.
		 */
		public function sendHold(value: Boolean): void
		{
			_trace("sendHold: " + value);
			if (currentState == ACTIVE) {
				_netConnection.call("hold", null, value);
			} else {
				_trace("Cannot hold() while state is " + currentState);
			}
		}

		/* RPC commands the server invokes on us */

		/**
		 * The callback is invoked by the server to indicate an incoming call from the
		 * given "from" user to this "to" user.
		 */
		public function invited(from: String, to: String): void
		{
			_trace("invited from {0} in line {1}", from, to);
			if (currentState == CONNECTED) {
				this.contactAddress = from;
				currentState = INBOUND;

				var e:Event = new PhoneEvent('PhoneEvent.OnInvited');
				e.from = from;
				e.to = to;
				dispatchEvent(e);
			} else {
				_trace("Missed call from {0} in line {1}", from, to);
				reject('486 Busy Here');
			}
		}

		/**
		 * The callback is invoked by the server to indicate that an incoming call is
		 * cancelled by the remote party.
		 */
		public function cancelled(): void
		{
			_trace("cancelled");
			if (currentState == INBOUND) {
				currentState = CONNECTED;
			}

			var e:Event = new PhoneEvent('PhoneEvent.OnCancelled');
			e.from = "";
			e.to = "";
			dispatchEvent(e);
		}

		/**
		 * The callback is invoked by the server to indicate that the call (incoming or outgoing)
		 * is finally established.
		 */
		public function accepted(audioCodec: String = null, videoCodec: String = null): void
		{
			_trace("accepted audioCodec={0} videoCodec={1}", audioCodec, videoCodec);
			if (currentState == OUTBOUND || currentState == INBOUND) {
				this.selectedAudio = audioCodec;
				this.selectedVideo = videoCodec;
				currentState = ACTIVE;
			}
			var e:Event = new PhoneEvent('PhoneEvent.OnAccepted');
			dispatchEvent(e);
		}

		/**
		 * The callback is invoked by the server to indicate that an outbound call
		 * failed for some reason.
		 */
		public function rejected(reason: String): void
		{
			_trace("rejected reason=" + reason);
			if (currentState == OUTBOUND) {
				currentState = CONNECTED;
			}
			var e:Event = new PhoneEvent('PhoneEvent.OnRejected');
			e.reason = reason;
			dispatchEvent(e);
		}

		/**
		 * The callback is invoked by the server to indicate the audio codec used for
		 * receiving audio has changed.
		 */
		public function codecChanged(newCodec: String): void
		{
			_trace("Audio codec changed from {0} to {1}", selectedAudio, newCodec);
			selectedAudio = newCodec;
			configureAudio();
		}

		/**
		 * The callback is invoked by the server to indicate that an active call is
		 * terminated by the remote party.
		 */
		public function byed(): void
		{
			_trace("byed");
			if (currentState == ACTIVE) {
				currentState = CONNECTED;
			}
			var e:Event = new PhoneEvent('PhoneEvent.OnByed');
			dispatchEvent(e);
		}

		/**
		 * When the remote side has put us on hold or unhold.
		 */
		public function onhold(value: Boolean): void
		{
			_trace("on hold: " + value);
			var e:Event = new PhoneEvent(value ? 'PhoneEvent.OnHold' : 'PhoneEvent.OnUnhold');
			dispatchEvent(e);
		}

		/**
		 * When the ringing event is received.
		 */
		public function ringing():void
		{
			_trace("ringing");
			var e:Event = new PhoneEvent('PhoneEvent.OnRinging');
			dispatchEvent(e);
		}

		/**
		 * When a DTMF digit is received.
		 */
		public function ondtmf(digit : String): void
		{
			_trace("(TODO) on dtmf: " + digit);
		}

		//--------------------------------------
		// PRIVATE METHODS
		//--------------------------------------

		private function _(format:String, ...args): String
		{
			var result:String = format;
			for (var i:int = 0; i < args.length; ++i) {
				result = result.replace("{" + i.toString() + "}", args[i] == null ? "null" : args[i].toString());
			}
			return result;
		}

		private function _trace(format:String, ...args): void
		{
			args.unshift(format);
			trace(" > " + _.apply(this, args));
		}

		private function netStatusHandler(event:NetStatusEvent):void
		{
			_trace("netStatusHandler() type={0} info.code={1}", event.type, event.info.code);

			switch (event.info.code) {
				case 'NetConnection.Connect.Success':
					_playStream    = new NetStream(_netConnection);
					_publishStream = new NetStream(_netConnection);

					_playStream.bufferTime = 0;
					_playStream.client = {}
					_publishStream.client = {}

					_playStream.addEventListener   (NetStatusEvent.NET_STATUS, netStatusHandler, false, 0, true);
					_publishStream.addEventListener(NetStatusEvent.NET_STATUS, netStatusHandler, false, 0, true);
					_playStream.addEventListener   (IOErrorEvent.IO_ERROR, errorHandler, false, 0, true);
					_publishStream.addEventListener(IOErrorEvent.IO_ERROR, errorHandler, false, 0, true);

					if (currentState == CONNECTING)
						currentState = CONNECTED;

					var e:Event = new PhoneEvent('PhoneEvent.OnConnected');
					dispatchEvent(e);
					break;
				case 'NetConnection.Connect.Failed':
				case 'NetConnection.Connect.Rejected':
				case 'NetConnection.Connect.Closed':
					disconnect();
					var error:Event = new PhoneEvent('PhoneEvent.OnError');
					error.code = event.info.code;
					error.description = 'description' in event.info ? event.info.description : event.info.code;
					dispatchEvent(error);
					break;
				default:
					break;
			}
		}

		/**
		 * When there is an error in the connection, close the connection and any associated stream.
		 */
		private function errorHandler(event: ErrorEvent):void
		{
			_trace("errorHandler() type={0} error.name={1} error.message={2} text={3}",
			        event.type, event.error.name, event.error.message, event.text);

			disconnect();

			var e:Event = new PhoneEvent('PhoneEvent.OnError');
			e.code = event.type;
			e.description = event.text;
			dispatchEvent(e);
		}

		/**
		 *
		 */
		private function configureAudio(): Microphone
		{
			if (selectedAudio == "ulaw") {
				selectedAudio = "pcmu";
			} else if (selectedAudio == "alaw") {
				selectedAudio = "pcma";
			} else { // force Speex
				selectedAudio = "Speex";
			}

			var mic:Microphone = Microphone.getMicrophone();
			mic.setSilenceLevel(0);
			mic.codec = selectedAudio;

			if (selectedAudio == "pcmu" || selectedAudio == "pcma") {
				mic.framesPerPacket = 2;
				mic.rate = 8;
			} else { // assume speex
				mic.framesPerPacket = 1;
				mic.encodeQuality = 6;
				mic.rate = 16;
			}

			return mic;
		}

		/**
		 * When the call is active, publish local stream and play remote stream.
		 */
		private function startPublishPlay():void
		{
			_trace("startPublishPlay");

			if (_publishStream != null) {
				_publishStream.publish("local");
				_publishStream.attachAudio(configureAudio());
				//_publishStream.attachCamera(Camera.getCamera());
			}

			if (_playStream != null) {
				_playStream.play("remote");
			}
		}

		/**
		 * When the call is terminated close both local and remote streams.
		 */
		private function stopPublishPlay():void
		{
			_trace("stopPublishPlay");

			if (_publishStream != null) {
				_publishStream.attachAudio(null);
				_publishStream.close();
			}

			if (_playStream != null) {
				_playStream.close();
			}
		}
	}
}
