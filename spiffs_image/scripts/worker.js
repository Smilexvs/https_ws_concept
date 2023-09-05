var root = null;
var newHandle = null;
var accessHandle = null;

async function init()
{
    root = await navigator.storage.getDirectory();
    newHandle = await root.getFileHandle("PatientData.txt", { "create" : true });
    accessHandle = await newHandle.createSyncAccessHandle();
}

var initFlag = false;
onmessage = async (event) =>
{
    if(initFlag == false)
    {
        await init();
        initFlag = true;
    }
    req = event.data;
    switch( req.type)
    {
    case "save":
        const encoder = new TextEncoder();
        const data = encoder.encode(JSON.stringify(event.data));
        const writeSize = accessHandle.write(data);
        accessHandle.flush();
    break;
    case "load":
        const fileSize = accessHandle.getSize();
        const readBuffer = new Int8Array(fileSize);
        const readSize = accessHandle.read(readBuffer, { at: 0 });
        const decoder = new TextDecoder();
        postMessage(decoder.decode(readBuffer));
    break;
    }
    
}
