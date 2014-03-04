package Phone 
{
	import flash.events.Event;
	
	public class PhoneEvent extends Event
	{
		public var code:String;
		public var description:String;
		public var from:String;
		public var to:String;
		public var reason:String;
		
		public function PhoneEvent(type:String, bubbles:Boolean = false, cancelable:Boolean = false) 
		{
			super(type, bubbles, cancelable);
		}
		
	}

}