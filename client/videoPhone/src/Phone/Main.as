package Phone 
{
	import flash.display.Sprite;
	import Phone.Connector;

	public class Main extends Sprite
	{
		[Bindable]
		/**
		 * The main data model and controller instance.
		 */
		public var connector:Connector = new Connector();
		
		public function Main() 
		{
			
		}
		
		public function get PhoneClient():Connector
		{
			return connector;
		}
	}

}