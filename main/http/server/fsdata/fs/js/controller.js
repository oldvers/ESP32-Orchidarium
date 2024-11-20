(function(exports)
{
    function Controller(options)
    {
        this.ws = NaN;
        this.retries = 0;
        this.onBeforeConnect = NaN;
        this.onConnected = NaN;
        this.onTimeout = NaN;
        this.onError = NaN;
        this.onConnectionParametersReceived = NaN;
        this.onStatusReceived = NaN;
        this.onDayMeasurementsReceived = NaN;
        this.onDisconnected = NaN;

        this.onWsOpen = this.onWsOpen.bind(this);
        this.onWsError = this.onWsError.bind(this);
        this.onWsMessage = this.onWsMessage.bind(this);
        this.onWsClose = this.onWsClose.bind(this);

        this._setMode = function(mode)
        {
            let buffer = new ArrayBuffer(2);
            let view = new Uint8Array(buffer);
        
            view[0] = Controller.Protocol.setSunImitationMode;
            if (Controller.Protocol.modeSunImitation == mode)
            {
                view[1] = Controller.Protocol.on;
            }
            else
            {
                view[1] = Controller.Protocol.off;
            }
    
            this.send(buffer);
            this.getStatus();
        }

        this._setValue = function(option, value)
        {
            let buffer = new ArrayBuffer(2);
            let view = new Uint8Array(buffer);
            view[0] = option;
            view[1] = value;
            this.send(buffer);
        }
    }

    Controller.Protocol =
    {
        getConnectionParameters: 0x01,
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
        retries: 5,
        modeSunImitation: 0,
        modeColor: 1,
        modeUnknown: 255
    };

    Controller.putStrInBuffer = function(view, offset, string)
    {
        let len = string.length;
        let offs = offset
        view.setUint8(offs++, len);
        for (let i = 0; i < len; i++, offs++)
        {
            view.setUint8(offs, string.charCodeAt(i));
        }
    }

    Controller.getStrFromBuffer = function(view, offset)
    {
        let offs = offset;
        let len  = view.getUint8(offs++);
        let str  = '';

        for (let i = 0; i < len; i++)
        {
            str += String.fromCharCode(view.getUint8(offs++));
        }

        return {value: str, length: (len + 1)};
    }

    Controller.prototype.send = function(data)
    {
        if ((WebSocket.CLOSED == this.ws.readyState) || 
            (Controller.Protocol.retries < this.retries++))
        {
            this.connect();
        }
        else if (WebSocket.OPEN == this.ws.readyState)
        {
            this.ws.send(data);
        }
        else if (WebSocket.CONNECTING == this.ws.readyState)
        {
            this.retries = 0;
        }
    }

    Controller.prototype.getStatus = function()
    {
        console.log("WS: Get Status");
        let buffer = new ArrayBuffer(1);
        let view = new Uint8Array(buffer);
        view[0] = Controller.Protocol.getStatus;
        this.send(buffer);
    }

    Controller.prototype.getDayMeasurements = function()
    {
        console.log("WS: Get Day Measurements");
        let buffer = new ArrayBuffer(1);
        let view = new Uint8Array(buffer);
        view[0] = Controller.Protocol.getDayMeasurements;
        this.send(buffer);
    }

    Controller.prototype.onMessageGetConnectionParameters = function (view)
    {
        console.log("WS: Configuration received!");

        let offset = 2;
        let ssid = Controller.getStrFromBuffer(view, offset);
        offset += ssid.length;
        let pwd = Controller.getStrFromBuffer(view, offset);
        offset += pwd.length;
        let site = Controller.getStrFromBuffer(view, offset);

        console.log("WS: Parameters: " + ssid.value + " " + pwd.value + " " + site.value);

        if (NaN != this.onConnectionParametersReceived)
        {
            this.onConnectionParametersReceived(ssid.value, pwd.value, site.value);
        }
    }

    Controller.prototype.onMessageGetStatus = function(view)
    {
        console.log("WS: Status received!");

        let mode   = view.getUint8(2);
        let color  = {r: view.getUint8(3), g: view.getUint8(4), b: view.getUint8(5)};
        let uv     = view.getUint8(6);
        let w      = view.getUint8(7);
        let fito   = view.getUint8(8);
        let fan    = view.getUint8(9);
        let hf     = view.getUint8(10);
        let p      = view.getUint32(11, true);
        let t      = view.getInt16(15, true);
        let h      = view.getUint16(17, true);
        let dts    = Controller.getStrFromBuffer(view, 19);
        let sun    = true;

        console.log("WS: Color = " + color.r + ":" + color.g + ":" + color.b + " - " + dts.value);

        if (Controller.Protocol.modeSunImitation != mode)
        {
            sun = false;
        }
        if (NaN != this.onStatusReceived)
        {
            this.onStatusReceived(sun, dts.value, color, uv, w, fito, fan, hf, p, t, h);
        }
    }

    Controller.prototype.onMessageGetDayMeasurements = function(view)
    {
        console.log("WS: Day Measurements received!");

        let t = 0.0;
        let ts = [];
        let h = 0.0;
        let hs = [];
        let p = 0.0;
        let ps = [];
        let offs = 2;
        let i = 0;

        for (i = 0; i < 24; i++)
        {
            p = (view.getUint32(offs, true) * 0.001);
            ps[i] = p.toFixed(1);
            offs += 4;
        }
        for (i = 0; i < 24; i++)
        {
            //t = (view.getInt16(offs, true) * 0.01);
            //ts[i] = t.toFixed(2);
            ts[i] = (view.getInt16(offs, true) * 0.01).toFixed(2);
            offs += 2;
        }
        for (i = 0; i < 24; i++)
        {
            h = (view.getUint16(offs, true) * 0.01);
            hs[i] = h.toFixed(2);
            offs += 2;
        }

        if (NaN != this.onDayMeasurementsReceived)
        {
            this.onDayMeasurementsReceived(ts, hs, ps);
        }
    }

    Controller.prototype.onWsOpen = function(event)
    {
        this.retries = 0;

        if (NaN != this.onConnected)
        {
            this.onConnected();
        }

        console.log("WS: Try to get the configuration...");
        
        let buffer = new ArrayBuffer(1);
        let view = new Uint8Array(buffer);

        view[0] = Controller.Protocol.getConnectionParameters;
        this.send(buffer);

        this.getStatus();
        this.getDayMeasurements();
    }

    Controller.prototype.onWsError = function(event)
    {
        if (NaN != this.onError)
        {
            this.onError();
        }
    }

    Controller.prototype.onWsMessage = function(event)
    {
        let view = new DataView(event.data);
        let command = view.getUint8(0);
        let status = view.getUint8(1);

        console.log("WS: Message from server!");

        this.retries = 0;

        if (Controller.Protocol.success == status)
        {
            if (Controller.Protocol.getConnectionParameters == command)
            {
                this.onMessageGetConnectionParameters(view);
            }
            else if (Controller.Protocol.setColor == command)
            {
                console.log("WS: Color is set!");
            }
            else if (Controller.Protocol.setSunImitationMode == command)
            {
                console.log("WS: The Sun imitation mode is set!");
            }
            else if (Controller.Protocol.getStatus == command)
            {
                this.onMessageGetStatus(view);
            }
            else if (Controller.Protocol.setUltraViolet == command)
            {
                console.log("WS: UltraViolet is set!");
            }
            else if (Controller.Protocol.setWhite == command)
            {
                console.log("WS: White is set!");
            }
            else if (Controller.Protocol.setFito == command)
            {
                console.log("WS: Fito is set!");
            }
            else if (Controller.Protocol.setFAN == command)
            {
                console.log("WS: FAN is set!");
            }
            else if (Controller.Protocol.setHumidifier == command)
            {
                console.log("WS: Humidifier is set!");
            }
            else if (Controller.Protocol.getDayMeasurements == command)
            {
                this.onMessageGetDayMeasurements(view);
            }
        }
    }

    Controller.prototype.onWsClose = function(event)
    {
        if (NaN != this.onDisconnected)
        {
            this.onDisconnected();
        }
    }

    Controller.prototype.connect = function()
    {
        if ((this.ws === undefined) || (this.ws == NaN) || (this.ws.readyState != WebSocket.CONNECTING))
        {
            if (Controller.Protocol.retries < this.retries)
            {
                if (NaN != this.onTimeout)
                {
                    this.onTimeout();
                }
            }
            else
            {
                if (NaN != this.onBeforeConnect)
                {
                    this.onBeforeConnect();
                }
            }
            console.log("location.host = " + location.host);
            if ((location.hostname === "localhost") ||
                (location.hostname === "127.0.0.1") ||
                (location.hostname === "fs"))
            {
                console.log("WS: Use local host");
                this.ws = new WebSocket("ws://localhost:8080");
            }
            else
            {
                console.log("WS: Use remote host");
                this.ws = new WebSocket("ws://" + location.host);
            }
            this.ws.binaryType = 'arraybuffer';
            this.ws.onopen = this.onWsOpen;
            this.ws.onerror = this.onWsError;
            this.ws.onmessage = this.onWsMessage;
            this.ws.onclose = this.onWsClose;
            this.retries = 0;
        }
    }

    Controller.prototype.setSunImitationMode = function(enabled)
    {
        console.log("WS: Set Sun Imitation Mode = " + enabled);
        if (true == enabled)
        {
            this._setMode(Controller.Protocol.modeSunImitation);
        }
        else
        {
            this._setMode(Controller.Protocol.modeColor);

        }
    }

    Controller.prototype.setColor = function(red, green, blue)
    {
        console.log("WS: Set color = " + red + ":" + green + ":" + blue);

        let buffer = new ArrayBuffer(4);
        let view = new Uint8Array(buffer);

        view[0] = Controller.Protocol.setColor;
        view[1] = red;
        view[2] = green;
        view[3] = blue;

        this.send(buffer);
    }

    Controller.prototype.setUltraViolet = function(value)
    {
        console.log("WS: Set UltraViolet = " + value);
        this._setValue(Controller.Protocol.setUltraViolet, value);
    }
    
    Controller.prototype.setWhite = function(value)
    {
        console.log("WS: Set White = " + value);
        this._setValue(Controller.Protocol.setWhite, value);
    }
    
    Controller.prototype.setFito = function(value)
    {
        console.log("WS: Set Fito = " + value);
        this._setValue(Controller.Protocol.setFito, value);
    }
    
    Controller.prototype.setFan = function(value)
    {
        console.log("WS: Set FAN = " + value);
        this._setValue(Controller.Protocol.setFAN, value);
    }
    
    Controller.prototype.setHumidifier = function(value)
    {
        console.log("WS: Set Humidifier = " + value);
        this._setValue(Controller.Protocol.setHumidifier, value);
    }

    //function wsOpenStream()
    //{
    //  var uri = "/stream"
    //  var ws = new WebSocket("ws://localhost:8080"); // + location.host + uri);
    //  ws.onmessage = function(evt)
    //  {
    //      console.log(evt.data);
    //      var stats = JSON.parse(evt.data);
    //      console.log(stats);
    //      document.getElementById('uptime').innerHTML = stats.uptime + ' seconds';
    //      document.getElementById('heap').innerHTML = stats.heap + ' bytes';
    //      document.getElementById('led').innerHTML = (stats.led == 1) ? 'On' : 'Off';
    //  };
    //}

    exports.Controller = Controller;
})(typeof exports === 'undefined' ? this : exports);