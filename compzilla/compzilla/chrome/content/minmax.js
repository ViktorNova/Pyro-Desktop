/* -*- mode: javascript; c-basic-offset: 4; indent-tabs-mode: t; -*- */

// poorly named file, but include MaximizeCompzillaFrame and
// RestoreCompzillaFrame here.

function MinimizeCompzillaFrame (w) {
    /* first we create a canvas of the right size/location */	
    var min = CompzillaWindowContent (w.content.nativeWindow);

    min.style.position = "absolute";

    min.style.left = (w.offsetLeft + w.content.offsetLeft) + "px";
    min.style.top = (w.offsetTop + w.content.offsetTop) + "px";
    min.style.width = w._contentBox.offsetWidth + "px";
    min.style.height = w._contentBox.offsetHeight + "px";
    min.width = w._contentBox.offsetWidth;
    min.height = w._contentBox.offsetHeight;

    Debug ("starting geometry = [" +
	   min.offsetLeft + ", " +
	   min.offsetTop + ", " +
	   min.offsetWidth + ", " +
	   min.offsetHeight + "]");

    /* insert the min window into the stack at the right level */
    w.parentNode.appendChild (min);
    min.style.zIndex = w.style.zIndex;

    /* hide the real window, show the min window */
    w.hide ();
    min.style.display = "block";

    var geom = w.content.getNativeProperty (Atoms._NET_WM_ICON_GEOMETRY);
    if (!geom) {
	/* for windows without the hint, iconify to the bottom left */
	geom = {
	    x: 0,
	    y: screen.height,
	    width: 0,
	    height: 0
	};
    }

    Debug ("minimize geometry = [" +
	   geom.x + ", " +
	   geom.y + ", " +
	   geom.width + ", " +
	   geom.height + "]");

    $(min).animate ({ left: geom.x, 
		      top: geom.y, 
		      width: geom.width, 
		      height: geom.height,
		      opacity: 0 },
		    1000, 
		    "easeout",
		    function () {
                        min.parentNode.removeChild (min);
                    });
}


var animate = true;


/* 
 * Custom jQuery animation that resizes the focused frame canvas with it's
 * current style size, to attempt to give a smooth transition while
 * maximizing/minimizing
 */
jQuery.easing["easeout-canvasresize"] = function(x, t, b, c, d) {
    if (_focusedFrame) {
	//_focusedFrame._updateContentSize ();
    }

    return jQuery.easing["easeout"](x, t, b, c, d);
}


function RestoreCompzillaFrame (w) {
    if (animate) {
	w.windowState = "normal";

	$(w).animate ({ left: w.restoreBounds.left, 
			top: w.restoreBounds.top, 
			width: w.restoreBounds.width, 
			height: w.restoreBounds.height },
		      500, 
		      "easeout-canvasresize",
		      function () {
			  w._updateContentSize ();
			  delete w.restoreBounds;
		      });
    }
    else {
	w.windowState = "normal";

	w.moveResize (w.restoreBounds.left,
		      w.restoreBounds.top,
		      w.restoreBounds.width,
		      w.restoreBounds.height);

	delete w.restoreBounds;
    }
}


function MaximizeCompzillaFrame (w) {
    if (animate) {
	$(w).animate ({ left: workarea.bounds.left, 
			top: workarea.bounds.top, 
			width: workarea.bounds.width, 
			height: workarea.bounds.height
		      },
		      500, // millis
		      "easeout-canvasresize",
		      function () {
			  w.windowState = "maximized";
			  w._updateContentSize ();
			  w.style.left = workarea.bounds.left;
			  w.style.top = workarea.bounds.top;
		      });
    }
    else {
	w.moveResize (workarea.bounds.left, 
		      workarea.bounds.top, 
		      workarea.bounds.width, 
		      workarea.bounds.height);
    }
}
