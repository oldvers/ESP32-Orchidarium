// Importing the required modules
const WebSocketServer = require('ws');
 
// Creating a new websocket server
const wss = new WebSocketServer.Server({ port: 8080 })

const wsProtocol = { getConnectionParameters: 0x01,
                     setConnectionParameters: 0x02,
                     setColor: 0x03,
                     setSunImitationMode: 0x04,
					 getStatus: 0x05,
                     success: 0x00,
                     on: 0x01,
                     off: 0x00,
                     modeSunImitation: 0,
                     modeColor: 1 };
var color = {mode: wsProtocol.modeSunImitation, r: 255, g: 255, b: 255};
				
function getStrFromBuffer(view, offset)
{
    let offs = offset;
    let len  = view[offs++];
    let str  = '';

    for (let i = 0; i < len; i++)
    {
        str += String.fromCharCode(view[offs++]);
    }

    return {value: str, length: (len + 1)};
}

function putStrInBuffer(view, offset, string)
{
    let len = string.length;
    let offs = offset;

    view[offs++] = len;
    for (let i = 0; i < len; i++, offs++)
    {
        view[offs] = string.charCodeAt(i);
    }
}

// Creating connection using websocket
wss.on("connection", ws => {
    console.log("New client connected");
 
    // sending message to client
    //ws.send('Welcome, you are connected!');
 
    //on message from client
    ws.on("message", data => {
        //console.log(`Client has sent us: ${data}`)
        
        let view = new Uint8Array(data);
        if (wsProtocol.getConnectionParameters == view[0])
        {
            console.log("Rx: Get connection parameters");

            let ssid = 'TestAccessPoint\0';
			let pwd = 'TestPassword\0';
			let site = 'testsite\0';

			let len = 5 + ssid.length + pwd.length + site.length;
			let offset = 0;
            let buffer = new ArrayBuffer(len);
            let view = new Uint8Array(buffer);

			view[offset] = wsProtocol.getConnectionParameters; /* Command */
			offset += (1);
            view[offset] = wsProtocol.success; /* Status */
            offset += (1);
			putStrInBuffer(view, offset, ssid);
			offset += (ssid.length + 1);
			putStrInBuffer(view, offset, pwd);
			offset += (pwd.length + 1);
			putStrInBuffer(view, offset, site);
				
			ws.send(buffer);
            
            console.log("Tx: Connection parameters");
            console.log("    - SSID: " + ssid + " (" + ssid.length + ")");
            console.log("    - PWD:  " + pwd + " (" + pwd.length + ")");
            console.log("    - Site: " + site + " (" + site.length + ")");
        }
        else if (wsProtocol.setConnectionParameters == view[0])
        {
            // Set connection parameters command
            console.log("Rx: Set connection parameters");
            
            let offset = 1;
            let ssid = getStrFromBuffer(view, offset);
            console.log("    - SSID: " + ssid.value + " (" + ssid.length + ")");
            offset += ssid.length;
            let pwd = getStrFromBuffer(view, offset);
            console.log("    - PWD:  " + pwd.value + " (" + pwd.length + ")");
            offset += pwd.length;
            let site = getStrFromBuffer(view, offset);
            console.log("    - Site: " + site.value + " (" + site.length + ")");
            
            let bytearray = new Uint8Array(2);
            ////for ( var i = 0; i < canvaspixellen; ++i ) {
            //var val = Math.floor(5 + Math.random() * 1014);
            bytearray[0] = wsProtocol.getConnectionParameters;
            bytearray[1] = wsProtocol.success;
            ws.send(bytearray.buffer);
        }
        else if (wsProtocol.setColor == view[0])
        {
            // Set color
			color.mode = wsProtocol.modeColor;
		    color.r = view[1];
			color.g = view[2];
			color.b = view[3];
			console.log("Rx: Set color - R:" + color.r + " G:" + color.g + " B:" + color.b);
			
            let bytearray = new Uint8Array(2);
            bytearray[0] = wsProtocol.setColor;
            bytearray[1] = wsProtocol.success;
            ws.send(bytearray.buffer);
        }
		else if (wsProtocol.setSunImitationMode == view[0])
		{
			// Set the Sun imitation mode
            console.log("Rx: Set the sun imitation mode: " + view[1]);
            
			if (wsProtocol.on == view[1])
			{
				color.mode = wsProtocol.modeSunImitation;
				color.r = 255;
				color.g = 255;
				color.b = 255;
			}
			else
			{
				color.mode = wsProtocol.modeColor;
			}
            let bytearray = new Uint8Array(2);
            bytearray[0] = wsProtocol.setSunImitationMode;
            bytearray[1] = wsProtocol.success;
            ws.send(bytearray.buffer);
		}
		else if (wsProtocol.getStatus == view[0])
		{
			// Get status
			let dt  = new Date();
			let dts = dt.toUTCString() + '\0';
            console.log("Rx: Get status - " + dts);
            
     		let len = 6 + dts.length;
			let offset = 0;
            let buffer = new ArrayBuffer(len);
            let view = new Uint8Array(buffer);

			view[offset] = wsProtocol.getStatus; /* Command */
			offset += (1);
			view[offset] = wsProtocol.success ; /* Status */
			offset += (1);
            view[offset] = color.mode; /* Mode */
            offset += (1);
			view[offset] = color.r; /* R */
            offset += (1);
			view[offset] = color.g; /* G */
            offset += (1);
			view[offset] = color.b; /* B */
            offset += (1);
			putStrInBuffer(view, offset, dts);
			
			ws.send(buffer);
		}
        
        //var bytearray = new Uint8Array( 2 );
        ////for ( var i = 0; i < canvaspixellen; ++i ) {
        //var val = Math.floor(5 + Math.random() * 1014);
        //bytearray[0] = (val >> 8) & 0xFF;
        //bytearray[1] = (val & 0xFF);
        //ws.send( bytearray.buffer );
    });
 
    // handling what to do when clients disconnects from server
    ws.on("close", () => {
        console.log("The client has disconnected");
    });
    // handling client connection error
    ws.onerror = function () {
        console.log("Some Error occurred")
    }
});

console.log("The WebSocket server is running on port: 8080");