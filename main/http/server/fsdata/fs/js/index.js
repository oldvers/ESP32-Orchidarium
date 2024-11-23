var rgbPicker = new iro.ColorPicker('#rgb_picker', { wheelLightness: false });
var intervalId = 0;
var mode = {last: false, initiatedByUser: false, needUiUpdate: false, width: 0};
var chart_t = undefined;
var chart_h = undefined;
var chart_p = undefined;
var series_t = new TimeSeries();
var series_h = new TimeSeries();
var series_p = new TimeSeries();
var controller = new Controller();
        
window.onload = onWindowLoad;

function createCharts()
{
    let grid = {fillStyle: '#ffffff', strokeStyle: '#d4d4d4', verticalSections: 4, millisPerLine: 1000, lineWidth: 1};
    let labels = {fillStyle: '#000000', fontSize: 11, precision: 3};
    let tooltip = {strokeStyle: '#bbbbbb'};
    let interval = 0;
    let canvas = undefined;

    chart_t = new SmoothieChart({grid: grid, labels: labels, tooltipLine: tooltip,
                                 maxValue: 40, minValue: 0,
                                 nonRealtimeData: true, interpolation: 'bezier',
                                 millisPerPixel: 56,
                                 responsive: true});
    chart_h = new SmoothieChart({grid: grid, labels: labels, tooltipLine: tooltip,
                                 maxValue: 100, minValue: 0,
                                 nonRealtimeData: true, interpolation: 'bezier',
                                 millisPerPixel: 56,
                                 responsive: true});
    chart_p = new SmoothieChart({grid: grid, labels: labels, tooltipLine: tooltip,
                                 maxValue: 110, minValue: 90,
                                 nonRealtimeData: true, interpolation: 'bezier',
                                 millisPerPixel: 56,
                                 responsive: true});
    
    canvas = document.getElementById("chart-t");
    chart_t.addTimeSeries(series_t, {lineWidth: 2, strokeStyle: '#cc8b00', fillStyle: 'rgba(204,139,0,0.18)'});
    chart_t.streamTo(canvas, 0);
    canvas = document.getElementById("chart-h");
    chart_h.addTimeSeries(series_h, {lineWidth: 2, strokeStyle: '#7542ff', fillStyle: 'rgba(117,66,255,0.18)'});
    chart_h.streamTo(canvas, interval);
    canvas = document.getElementById("chart-p");
    chart_p.addTimeSeries(series_p, {lineWidth: 2, strokeStyle: '#01a201', fillStyle: 'rgba(0,163,0,0.18)'});
    chart_p.streamTo(canvas, interval);
}

function onWindowLoad()
{
    console.log("Window loaded");

    rgbPicker.on('input:start', onStartColorChange);
    rgbPicker.on('input:end', onEndColorChange);
    uv_picker = document.getElementById('uv_picker');
    uv_picker.addEventListener("change", onUvChange);
    uv_picker.addEventListener("input", onInput);
    w_picker = document.getElementById('w_picker');
    w_picker.addEventListener("change", onWChange);
    w_picker.addEventListener("input", onInput);
    fito_picker = document.getElementById('fito_picker');
    fito_picker.addEventListener("change", onFitoChange);
    fito_picker.addEventListener("input", onInput);
    fan_picker = document.getElementById('fan_picker');
    fan_picker.addEventListener("change", onFanChange);
    fan_picker.addEventListener("input", onInput);
    hf_picker = document.getElementById('hf_picker');
    hf_picker.addEventListener("change", onHfChange);
    hf_picker.addEventListener("input", onInput);
    sun_switch = document.getElementById("sun_switch");
    sun_switch.onclick = onSunSwitchClick;

    createCharts();

    controller.onBeforeConnect = onBeforeConnect;
    controller.onConnected = onConnected;
    controller.onTimeout = onTimeout;
    controller.onError = onError;
    controller.onConnectionParametersReceived = onConnectionParametersReceived;
    controller.onStatusReceived = onStatusReceived;
    controller.onDayMeasurementsReceived = onDayMeasurementsReceived;
    controller.onDisconnected = onDisconnected;
    controller.connect();

    intervalId = setInterval(getStatus, 1000);
    lt = document.getElementById('lt');
    lt.innerText = "24.4 \xB0";

    mode.width = document.getElementById("chart-t").width;
}

function getStatus()
{
    controller.getStatus();
}

function updateStatusBox(cls, text)
{
    sbox = document.getElementById('status_box');
    sbox.className = "alert is-" + cls;
    slabel = document.getElementById('status');
    slabel.innerText = text;
}

function uiUpdate(color, uv, w, fito, fan, hf)
{
    rgbPicker.color.set(color);
    document.getElementById('uv_picker').value = uv;
    document.getElementById('w_picker').value = w;
    document.getElementById('fito_picker').value = fito;
    document.getElementById('fan_picker').value = fan;
    document.getElementById('hf_picker').value = hf;
}

function onBeforeConnect()
{
    updateStatusBox("secondary", "Connecting...");
}

function onConnected(event)
{
    updateStatusBox("success", "Connected");
}

function onTimeout()
{
    updateStatusBox("danger", "Timeout!");
}

function onError()
{
    updateStatusBox("danger", "Error!");
}

function onDisconnected()
{
    updateStatusBox("warning", "Disconnected");
}

function onConnectionParametersReceived(ssid, pwd, site)
{
    //document.getElementById('site').innerText = site.value;
}

function onStatusReceived(sun, dts, color, uv, w, fito, fan, hf, p, t, h, r)
{
    updateStatusBox("success", dts);

    let temp = (t * 0.01);
    document.getElementById("lt").innerText = temp.toFixed(2).toString() + " \xB0";
    let hum = (h * 0.01);
    document.getElementById("lh").innerText = hum.toFixed(2).toString() + " %";
    let press = (p * 0.001);
    document.getElementById("lp").innerText = press.toFixed(1).toString() + " kP";

    sun_switch = document.getElementById("sun_switch");

    if ((true == sun) && (true == sun_switch.checked))
    {
        uiUpdate(color, uv, w, fito, fan, hf);
    }
    if (sun != mode.last)
    {
        mode.last = sun;
        if (false == mode.initiatedByUser)
        {
            mode.needUiUpdate = true;
        }
    }
    if (true == mode.needUiUpdate)
    {
        if (true == sun)
        {
            sun_switch.checked = true;
        }
        else
        {
            sun_switch.checked = false;
        }
        uiUpdate(color, uv, w, fito, fan, hf);
        mode.needUiUpdate = false;
    }
    let canvas = document.getElementById("chart-t");
    if ((mode.width != canvas.width) || (true == r))
    {
        controller.getDayMeasurements();
        mode.width = canvas.width;
    }
}

function onDayMeasurementsReceived(ts, hs, ps)
{
    let i = 0;
    console.log("Day Measurements: " + ts.length);

    let canvas = document.getElementById("chart-t");
    let milisPerPixel = (23000 * window.devicePixelRatio / canvas.width);

    chart_t.options.millisPerPixel = milisPerPixel;
    chart_h.options.millisPerPixel = milisPerPixel;
    chart_p.options.millisPerPixel = milisPerPixel;

    series_t.clear();
    for (i = 0; i < ts.length; i++)
    {
        series_t.append(i * 1000.0, ts[i]);
    }
    series_h.clear();
    for (i = 0; i < hs.length; i++)
    {
        series_h.append(i * 1000.0, hs[i]);
    }
    series_p.clear();
    for (i = 0; i < ps.length; i++)
    {
        series_p.append(i * 1000.0, ps[i]);
    }
}

function onSunSwitchClick()
{
    controller.setSunImitationMode(this.checked);
}

function onStartColorChange(color)
{
    document.getElementById("sun_switch").checked = false;
    mode.initiatedByUser = true;
}

function onEndColorChange(color)
{
    controller.setColor(color.red, color.green, color.blue);
}

function onInput(event)
{
    sun_switch = document.getElementById("sun_switch")
    if (true == sun_switch.checked)
    {
        sun_switch.checked = false;
        mode.initiatedByUser = true;
    }
}
        
function onUvChange(event)
{
    controller.setUltraViolet(event.target.value);
}

function onWChange(event)
{
    controller.setWhite(event.target.value);
}

function onFitoChange(event)
{
    controller.setFito(event.target.value);
}

function onFanChange(event)
{
    controller.setFan(event.target.value);
}

function onHfChange(event)
{
    controller.setHumidifier(event.target.value);
}
