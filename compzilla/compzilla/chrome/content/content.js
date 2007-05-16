/* -*- mode: javascript; c-basic-offset: 4; indent-tabs-mode: t; -*- */


function CompzillaWindowContent (nativewin) {
    Debug ("Creating content for nativewin=" + nativewin);

    var content = document.getElementById ("windowContent").cloneNode (true);

    content._nativewin = nativewin;
    content.getNativeWindow = function () { return content._nativewin; }

    content.destroy = function () { 
	Debug ("content.destroy");
	content._nativewin.RemoveContentNode (content); 
    }

    nativewin.AddContentNode (content);
    return content;
}


