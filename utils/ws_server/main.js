/* Importing the required modules */
const WebSocketServer = require('ws');
 
/* Creating a new websocket server */
const wss = new WebSocketServer.Server({ port: 8080 })

const wsProtocol = { getConnectionParameters: 0x01,
                     setConnectionParameters: 0x02,
                     setColor: 0x03,
                     setSunImitationMode: 0x04,
					 getStatus: 0x05,
                     setUltraViolet: 0x06,
					 setWhite: 0x07,
					 setFito: 0x08,
					 setFAN: 0x09,
					 setHumidifier: 0x0A,
                     getDayMeasurements: 0x0B,
                     success: 0x00,
                     on: 0x01,
                     off: 0x00,
                     modeSunImitation: 0,
                     modeColor: 1 };

var color = { mode: wsProtocol.modeSunImitation, r: 255, g: 255, b: 255 };
				
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

    view.setUint8(offs++, len);
    for (let i = 0; i < len; i++, offs++)
    {
        view.setUint8(offs, string.charCodeAt(i));
    }
}

/* Creating connection using websocket */
wss.on("connection", ws =>
{
    console.log("New client connected");
 
    function onGetConnectionParameters()
    {
        console.log("Rx: Get connection parameters");

        let ssid = 'TestAccessPoint\0';
        let pwd = 'TestPassword\0';
        let site = 'testsite\0';

        let len = 5 + ssid.length + pwd.length + site.length;
        let offset = 0;
        let buffer = new ArrayBuffer(len);
        let view = new DataView(buffer);

        view.setUint8(offset++, wsProtocol.getConnectionParameters); /* Command */
        view.setUint8(offset++, wsProtocol.success); /* Status */
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

    function onSetConnectionParameters(uint8view)
    {
        console.log("Rx: Set connection parameters");
        
        let offset = 1;
        let ssid = getStrFromBuffer(uint8view, offset);
        console.log("    - SSID: " + ssid.value + " (" + ssid.length + ")");
        offset += ssid.length;
        let pwd = getStrFromBuffer(uint8view, offset);
        console.log("    - PWD:  " + pwd.value + " (" + pwd.length + ")");
        offset += pwd.length;
        let site = getStrFromBuffer(uint8view, offset);
        console.log("    - Site: " + site.value + " (" + site.length + ")");
        
        let bytearray = new Uint8Array(2);
        bytearray[0] = wsProtocol.getConnectionParameters;
        bytearray[1] = wsProtocol.success;
        ws.send(bytearray.buffer);
    }
 
    function onSetColor(uint8view)
    {
        color.mode = wsProtocol.modeColor;
        color.r = uint8view[1];
        color.g = uint8view[2];
        color.b = uint8view[3];
        console.log("Rx: Set color - R:" + color.r + " G:" + color.g + " B:" + color.b);

        let bytearray = new Uint8Array(2);
        bytearray[0] = wsProtocol.setColor;
        bytearray[1] = wsProtocol.success;
        ws.send(bytearray.buffer);
    }

    function onSetSunImitationMode(uint8view)
    {
        console.log("Rx: Set the sun imitation mode: " + uint8view[1]);
         
        if (wsProtocol.on == uint8view[1])
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

    function onGetStatus()
    {
        let dt  = new Date();
        let dts = dt.toUTCString();
        console.log("Rx: Get status - " + dts);

        let len = (19 + 1 + dts.length);
        let buffer = new ArrayBuffer(len);
        let view = new DataView(buffer);

        view.setUint8(0, wsProtocol.getStatus); /* Command */
        view.setUint8(1, wsProtocol.success); /* Status */
        view.setUint8(2, color.mode); /* Mode */
        view.setUint8(3, color.r); /* R */
        view.setUint8(4, color.g); /* G */
        view.setUint8(5, color.b); /* B */
        view.setUint8(6, 80); /* UV */
        view.setUint8(7, 160); /* W */
        view.setUint8(8, 240); /* Fito */
        view.setUint8(9, 2); /* FAN */
        view.setUint8(10, 1); /* Humidifier */
        view.setUint32(11, (90000 + Math.random() * 20000), true); /* Pressure */
        view.setInt16(15, (Math.random() * 3500), true); /* Temperature */
        view.setUint16(17, (Math.random() * 9500), true); /* Humidity */
        putStrInBuffer(view, 19, dts); /* Date-time string */

        ws.send(buffer);
    }

    function onSetUltraViolet(uint8view)
    {
        color.mode = wsProtocol.modeColor;
        console.log("Rx: Set UV: " + uint8view[1]);
        let bytearray = new Uint8Array(2);
        bytearray[0] = wsProtocol.setUltraViolet;
        bytearray[1] = wsProtocol.success;
        ws.send(bytearray.buffer);

    }

    function onSetWhite(uint8view)
    {
        color.mode = wsProtocol.modeColor;
        console.log("Rx: Set W: " + uint8view[1]);
        let bytearray = new Uint8Array(2);
        bytearray[0] = wsProtocol.setWhite;
        bytearray[1] = wsProtocol.success;
        ws.send(bytearray.buffer);
    }

    function onSetFito(uint8view)
    {
        color.mode = wsProtocol.modeColor;
        console.log("Rx: Set Fito: " + uint8view[1]);
        let bytearray = new Uint8Array(2);
        bytearray[0] = wsProtocol.setFito;
        bytearray[1] = wsProtocol.success;
        ws.send(bytearray.buffer);
    }

    function onSetFAN(uint8view)
    {
        color.mode = wsProtocol.modeColor;
        console.log("Rx: Set FAN: " + uint8view[1]);
        let bytearray = new Uint8Array(2);
        bytearray[0] = wsProtocol.setFAN;
        bytearray[1] = wsProtocol.success;
        ws.send(bytearray.buffer);
    }

    function onSetHumidifier(uint8view)
    {
        color.mode = wsProtocol.modeColor;
        console.log("Rx: Set Humidifier: " + uint8view[1]);
        let bytearray = new Uint8Array(2);
        bytearray[0] = wsProtocol.setHumidifier;
        bytearray[1] = wsProtocol.success;
        ws.send(bytearray.buffer);
    }

    function onGetDayMeasurements()
    {
        console.log("Rx: Get Day Measurements");

        let len = (2 + 24 * 4 + 24 * 2 + 24 * 2);
        let buffer = new ArrayBuffer(len);
        let view = new DataView(buffer);
        let offs = 0;
        let i = 0;

        view.setUint8(offs++, wsProtocol.getDayMeasurements); /* Command */
        view.setUint8(offs++, wsProtocol.success); /* Status */
        for (i = 0; i < 24; i++)  /* Pressure */
        {
            view.setUint32(offs, (90000 + Math.random() * 20000), true);
            offs += 4;
        }
        for (i = 0; i < 24; i++) /* Temperature */
        {
            view.setInt16(offs, (Math.random() * 3500), true);
            offs += 2;
        }
        for (i = 0; i < 24; i++) /* Humidity */
        {
            view.setUint16(offs, (Math.random() * 9500), true);
            offs += 2;
        }

        ws.send(buffer);
    }

    ws.on("message", data =>
    {
        let view = new Uint8Array(data);
        let command = view[0];

        if (wsProtocol.getConnectionParameters == command)
        {
            onGetConnectionParameters();
        }
        else if (wsProtocol.setConnectionParameters == command)
        {
            onSetConnectionParameters(view);
        }
        else if (wsProtocol.setColor == command)
        {
            onSetColor(view);
        }
		else if (wsProtocol.setSunImitationMode == command)
		{
            onSetSunImitationMode(view);
		}
		else if (wsProtocol.getStatus == command)
		{
            onGetStatus();
		}
        else if (wsProtocol.setUltraViolet == command)
        {
            onSetUltraViolet(view);
        }
        else if (wsProtocol.setWhite == command)
        {
            onSetWhite(view);
        }
        else if (wsProtocol.setFito == command)
        {
            onSetFito(view);
        }
        else if (wsProtocol.setFAN == command)
        {
            onSetFAN(view);
        }
        else if (wsProtocol.setHumidifier == command)
        {
            onSetHumidifier(view);
        }
        else if (wsProtocol.getDayMeasurements == command)
        {
            onGetDayMeasurements();
        }
    });
 
    ws.on("close", () =>
    {
        console.log("The client has disconnected");
    });

    ws.onerror = function ()
    {
        console.log("Some Error occurred")
    }
});

console.log("The WebSocket server is running on port: 8080");