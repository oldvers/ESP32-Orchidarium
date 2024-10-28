# Chrome as a Local Web Server
The Chrome Developer Tools can be used as a versatile tool for web development. They allow overriding resources used by web pages and replacing them with the own local resources. Any change made to local web page source file is reflected live in the web browser.

The Chrome local override feature can be used to create a web server to serve static web pages. And name can be used as the domain name of the web server. Even if the domain name already exists, Chrome will serve pages from the local web server if the requested page or resource exists.

# Developing the device's Web interface
1. Load the Chrome and open Developer Tools window by pressing F12.
2. Click "Sources" tab and then select "Overrides" sub-tab on the left navigation panel.
3. Click "+ Select folder for overrides".
4. In the folder selection dialog, select [fsdata](../main/http/server/fsdata) as the root of all overrides.
5. When Chrome asks to confirm access, click "Allow" to let Chrome access the folder contents.
6. The folder and contents should appear in the navigation panel.
7. In the browser window enter "fs/" to the address field.
8. The device's web page should appear.
9. Modify the web page source in the "Sources" tab.
10. Save the modifications.
11. Click "Refresh" button in the browser.
12. The modified device's web page should appear.

# WebSocket server
1. Install the [Node.js](https://nodejs.org).
2. Open the command line shell.
3. Navigate to the "[ws_server](../utils/ws_server/)" folder.
4. Install the "ws" module - enter "**npm install ws**" to the command line.
5. Run the WebSocket server - enter "**node main.js**" to the command line.
6. Now the WebSocket server can be used together with the Chrome local Web server to debug the communication between the web page and the WebSocket server.

# Updating the Web interface on the device
1. Run the [makefsdata.py](../main/http/server/fsdata/makefsdata.py) script to regenerate the C source file with the web content.
2. Rebuild the C project and reflash it to the device.
3. Wait till device is connected to the WiFi router.
4. Reopen the device's web page in the browser.
5. Check if the modifications work on the device.