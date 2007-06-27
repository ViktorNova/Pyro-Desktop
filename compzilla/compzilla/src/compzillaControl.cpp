/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil; -*- */

#include <nspr.h>
#include <jsapi.h>
#include <nsMemory.h>
#include <nsHashPropertyBag.h>
#include <nsIBaseWindow.h>
#include <nsIDocShell.h>
#include <nsIScriptGlobalObject.h>
#ifndef MOZILLA_1_8_BRANCH
#include <nsPIDOMWindow.h>
#endif
#include <nsIWidget.h>

#include "compzillaControl.h"
#include "XAtoms.h"

extern "C" {
#include <gdk/gdkx.h>
#include <gdk/gdkdisplay.h>
#include <gdk/gdkscreen.h>

#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xrender.h>

#define HAVE_XEVIE 0
#define HAVE_XEVIE_WRITEONLY 0
#if HAVE_XEVIE || HAVE_XEVIE_WRITEONLY
#include <X11/extensions/Xevie.h>
#endif
}


#ifdef DEBUG
#ifdef WITH_SPEW
#define SPEW(format...) printf("   - " format)
#else
#define SPEW(format...)
#endif
#define INFO(format...) printf(" *** " format)
#define WARNING(format...) printf(" !!! " format)
#define ERROR(format...) fprintf(stderr, format)
#else
#define SPEW(format...) do { } while (0)
#define INFO(format...) do { } while (0)
#define WARNING(format...) do { } while (0)
#define ERROR(format...) do { } while (0)
#endif


NS_IMPL_ISUPPORTS1_CI(compzillaControl, compzillaIControl);


int compzillaControl::sErrorCnt;
int compzillaControl::composite_event;
int compzillaControl::composite_error;
int compzillaControl::xevie_event;
int compzillaControl::xevie_error;
int compzillaControl::damage_event;
int compzillaControl::damage_error;
int compzillaControl::xfixes_event;
int compzillaControl::xfixes_error;
int compzillaControl::shape_event;
int compzillaControl::shape_error;
int compzillaControl::render_event;
int compzillaControl::render_error;


XAtoms atoms;

compzillaControl::compzillaControl()
{
    mWindowMap.Init(50);
}


compzillaControl::~compzillaControl()
{
    if (mRoot) {
        gdk_window_unref(mRoot);
    }
    if (mMainwin) {
        gdk_window_unref(mMainwin);
    }

#if HAVE_XEVIE || HAVE_XEVIE_WRITEONLY
    // Stop intercepting mouse/keyboard events
    XevieEnd (mXDisplay);
#endif
}


/*
 *
 * compzillaIControl Implementation...
 *
 */

NS_IMETHODIMP
compzillaControl::RegisterWindowManager(nsIDOMWindow *window)
{
    Display *dpy;
    nsresult rv = NS_OK;

    SPEW ("RegisterWindowManager\n");

    mDOMWindow = window;

    mRoot = gdk_get_default_root_window();
    mXRoot = GDK_DRAWABLE_XID (mRoot);
    mXDisplay = GDK_DISPLAY_XDISPLAY (gdk_display_get_default ());

    // Get ALL events for ALL windows
    gdk_window_add_filter (NULL, gdk_filter_func, this);

    // Just ignore errors for now
    XSetErrorHandler (ErrorHandler);

    InitXAtoms ();

    // Check extension versions
    if (!InitXExtensions ()) {
        ERROR ("InitXExtensions failed");
        exit (1); // return NS_ERROR_UNEXPECTED;
    }

    // Take over drawing
    if (!InitOutputWindow ()) {
        ERROR ("InitOutputWindow failed");
        exit (1); // return NS_ERROR_UNEXPECTED;
    }

    // Register as window manager
    if (!InitWindowState ()) {
        ERROR ("InitWindowState failed");
        exit (1); // return NS_ERROR_UNEXPECTED;
    }

    return rv;
}


NS_IMETHODIMP
compzillaControl::InternAtom (const char *property, PRUint32 *value)
{
    *value = (PRUint32) XInternAtom (mXDisplay, property, FALSE);
    return NS_OK;
}


NS_IMETHODIMP
compzillaControl::SetRootWindowProperty (PRInt32 prop, 
                                         PRInt32 type, 
                                         PRUint32 count, 
                                         PRUint32* valueArray)
{
    XChangeProperty (mXDisplay, mXRoot,
                     prop, type, 32,
                     PropModeReplace,
                     (unsigned char*)valueArray, count);

#ifdef OVERLAY_INPUT_REGION
    if (prop == atoms.x._NET_ACTIVE_WINDOW && 
        type == XA_WINDOW) {
        nsRefPtr<compzillaWindow> compwin = FindWindow (valueArray[0]);
        SetOverlayInput (compwin);
    }
#endif /* OVERLAY_INPUT_REGION */

    return NS_OK;
}


NS_IMETHODIMP
compzillaControl::DeleteRootWindowProperty (PRInt32 prop)
{
    XDeleteProperty (mXDisplay, mXRoot,
                     prop);

    return NS_OK;
}


NS_IMETHODIMP
compzillaControl::SendConfigureNotify (PRUint32 xid,
                                       PRUint32 x, PRUint32 y,
                                       PRUint32 width, PRUint32 height,
                                       PRUint32 border,
                                       PRBool overrideRedirect)
{
    XEvent ev;

    memset (&ev, 0, sizeof (ev));

    ev.type = ConfigureNotify;
    ev.xconfigure.display = mXDisplay;
    ev.xconfigure.window = xid;
    ev.xconfigure.event = xid;
    ev.xconfigure.x = x;
    ev.xconfigure.y = y;
    ev.xconfigure.width = width;
    ev.xconfigure.height = height;
    ev.xconfigure.border_width = border;
    ev.xconfigure.above = None;
    ev.xconfigure.override_redirect = overrideRedirect;

    SPEW ("SendConfigureNotify (window=%p, x=%d, y=%d, width=%d, height=%d, "
          "border=%d, override=%d)\n",
          ev.xconfigure.window,
          ev.xconfigure.x,
          ev.xconfigure.y,
          ev.xconfigure.width,
          ev.xconfigure.height,
          ev.xconfigure.border_width,
          ev.xconfigure.override_redirect);

    XSendEvent (mXDisplay, xid, False, StructureNotifyMask, &ev);
}


NS_IMETHODIMP
compzillaControl::MoveToTop (PRUint32 xid)
{
    XWindowChanges changes;

    changes.sibling = None;
    changes.stack_mode = Above;

    XConfigureWindow (mXDisplay, xid,
                      CWStackMode,
                      &changes);
}


NS_IMETHODIMP
compzillaControl::MoveToBottom (PRUint32 xid)
{
    XWindowChanges changes;

    changes.sibling = None;
    changes.stack_mode = Below;

    XConfigureWindow (mXDisplay, xid,
                      CWStackMode,
                      &changes);
}


NS_IMETHODIMP
compzillaControl::ConfigureWindow (PRUint32 xid,
                                   PRUint32 x, PRUint32 y,
                                   PRUint32 width, PRUint32 height,
                                   PRUint32 border)
{
    XWindowChanges changes;

    changes.x = x;
    changes.y = y;
    changes.width = width;
    changes.height = height;
    changes.border_width = border;

    SPEW ("ConfigureWindow calling XConfigureWindow (window=%p, x=%d, y=%d, "
          "width=%d, height=%d, border=%d)\n",
          xid, x, y, width, height, border);

    XConfigureWindow (mXDisplay, xid,
                      (CWX | CWY | CWWidth | CWHeight | CWBorderWidth),
                      &changes);
}


NS_IMETHODIMP
compzillaControl::MapWindow (PRUint32 xid)
{
    XMapWindow (mXDisplay, xid);
}


NS_IMETHODIMP
compzillaControl::UnmapWindow (PRUint32 xid)
{
    XUnmapWindow (mXDisplay, xid);
}


NS_IMETHODIMP 
compzillaControl::AddObserver (compzillaIControlObserver *aObserver)
{
    SPEW ("compzillaWindow::AddObserver %p - %p\n", this, aObserver);

    RemoveObserver (aObserver);
    mObservers.AppendObject (aObserver);

    /* 
     * When initially adding an observer, call windowCreate for all existing
     * windows?
     */

    return NS_OK;
}


NS_IMETHODIMP 
compzillaControl::RemoveObserver (compzillaIControlObserver *aObserver)
{
    SPEW ("compzillaControl::RemoveObserver %p\n", this);

    // Allow a caller to remove O(N^2) behavior by removing end-to-start.
    for (PRUint32 i = mObservers.Count() - 1; i != PRUint32(-1); --i) {
        if (mObservers.ObjectAt(i) == aObserver) {
            mObservers.RemoveObjectAt (i);
            return NS_OK;
        }
    }

    return NS_ERROR_FAILURE;
}


/*
 *
 * Private methods...
 *
 */

GdkFilterReturn
compzillaControl::gdk_filter_func (GdkXEvent *xevent, GdkEvent *event, gpointer data)
{
    compzillaControl *control = reinterpret_cast<compzillaControl*>(data);
    return control->Filter (xevent, event);
}


nsresult
compzillaControl::GetNativeWidget(nsIDOMWindow *window, nsIWidget **widget)
{
#ifdef MOZILLA_1_8_BRANCH
    nsCOMPtr<nsIScriptGlobalObject> global = do_QueryInterface(window);
#else
    nsCOMPtr<nsPIDOMWindow> global = do_QueryInterface(window);
#endif

    if (global) {
        nsCOMPtr<nsIBaseWindow> baseWin = do_QueryInterface(global->GetDocShell());
        if (baseWin) {
            baseWin->GetMainWidget(widget);
            return NS_OK;
        }
    }

    return NS_ERROR_FAILURE;
}


GdkWindow *
compzillaControl::GetNativeWindow(nsIDOMWindow *window)
{
    nsCOMPtr<nsIWidget> widget;

    if (NS_OK == GetNativeWidget (window, getter_AddRefs (widget))) {
        GdkWindow *iframe = (GdkWindow *)widget->GetNativeData (NS_NATIVE_WINDOW);
        GdkWindow *toplevel = gdk_window_get_toplevel (iframe);

        SPEW ("GetNativeWindow: toplevel=0x%0x, iframe=0x%0x\n", 
              GDK_DRAWABLE_XID (toplevel), GDK_DRAWABLE_XID (iframe));
        return toplevel;
    }

    WARNING ("Could not get GdkWindow for nsIDOMWindow %p\n", window);
    return NULL;
}


bool
compzillaControl::InitXAtoms ()
{
    XInternAtoms (mXDisplay, 
                  atom_names, sizeof (atom_names) / sizeof (atom_names[0]), 
                  False, 
                  atoms.a);
    return true;
}


bool
compzillaControl::InitXExtensions ()
{
#define ERR_RET(_str) do { ERROR (_str); return false; } while (0)

    if (!XRenderQueryExtension (mXDisplay, &render_event, &render_error)) {
	ERR_RET ("No render extension\n");
    }

    int opcode;
    if (!XQueryExtension (mXDisplay, COMPOSITE_NAME, &opcode, 
                          &composite_event, &composite_error)) {
	ERR_RET ("No composite extension\n");
    }

    int	composite_major, composite_minor;
    XCompositeQueryVersion (mXDisplay, &composite_major, &composite_minor);
    if (composite_major == 0 && composite_minor < 2) {
        ERR_RET ("Old composite extension does not support XCompositeGetOverlayWindow\n");
    }
    SPEW ("composite extension: major = %d, minor = %d, opcode = %d, event = %d, error = %d\n",
           composite_major, composite_minor, opcode, composite_event, composite_error);

    if (!XDamageQueryExtension (mXDisplay, &damage_event, &damage_error)) {
	ERR_RET ("No damage extension\n");
    }
    SPEW ("damage extension: event = %d, error = %d\n",
           damage_event, damage_error);

#if HAVE_XEVIE || HAVE_XEVIE_WRITEONLY
    if (!XQueryExtension (mXDisplay, "XEVIE", &opcode, &xevie_event, &xevie_error)) {
	ERR_RET ("No Xevie extension\n");
    }

    int evie_major, evie_minor;
    if (!XevieQueryVersion (mXDisplay, &evie_major, &evie_minor)) {
	ERR_RET ("No Xevie extension\n");
    }
    SPEW ("xevie extension: major = %d, minor = %d, opcode = %d, event = %d, error = %d\n",
           evie_major, evie_minor, opcode, evie_event, evie_error);
#endif

    if (!XFixesQueryExtension (mXDisplay, &xfixes_event, &xfixes_error)) {
	ERR_RET ("No XFixes extension\n");
    }
    SPEW ("xfixes extension: event = %d, error = %d\n",
           xfixes_event, xfixes_error);

    if (!XShapeQueryExtension (mXDisplay, &shape_event, &shape_error)) {
	ERR_RET ("No Shaped window extension\n");
    }
    SPEW ("shape extension: event = %d, error = %d\n",
           shape_event, shape_error);

#undef ERR_RET

    return true;
}


bool
compzillaControl::InitWindowState ()
{
    if (!InitManagerWindow ())
        return FALSE;

    XGrabServer (mXDisplay);

    long ev_mask = (SubstructureRedirectMask |
                    SubstructureNotifyMask |
                    StructureNotifyMask |
                    PropertyChangeMask);
    XSelectInput (mXDisplay, GDK_WINDOW_XID (mRoot), ev_mask);

    if (ClearErrors (mXDisplay)) {
        WARNING ("Another window manager is already running on screen: %d\n", 1);

        ev_mask &= ~(SubstructureRedirectMask);
        XSelectInput (mXDisplay, GDK_WINDOW_XID (mRoot), ev_mask);
    }

#if HAVE_XEVIE
    // Intercept mouse/keyboard events
    XevieStart (mXDisplay);
    XevieSelectInput (mXDisplay, (ButtonPressMask | 
                                  ButtonReleaseMask |
                                  KeyPressMask | 
                                  KeyReleaseMask));
#elif HAVE_XEVIE_WRITEONLY
    XevieStart (mXDisplay);
    //XevieSelectInput (mXDisplay, NoEventMask);
#endif

    if (mIsWindowManager) {
        // Start managing any existing windows
        Window root_notused, parent_notused;
        Window *children;
        unsigned int nchildren;

        XQueryTree (mXDisplay, 
                    mXRoot, 
                    &root_notused, 
                    &parent_notused, 
                    &children, 
                    &nchildren);

        for (int i = 0; i < nchildren; i++) {
            if (children[i] != mOverlay
                && children[i] != GDK_WINDOW_XID (mMainwin))
            AddWindow (children[i]);
        }

        XFree (children);
    }

    XUngrabServer (mXDisplay);

    return true;
}


bool
compzillaControl::InitManagerWindow ()
{
    SPEW ("InitManagerWindow\n");

    XSetWindowAttributes attrs;

    attrs.override_redirect = True;
    attrs.event_mask = PropertyChangeMask;
  
    // FIXME: Do this for each screen, and don't kill existing WMs.  See
    //        compiz's display.c:addDisplay.

    mManagerWindow = XCreateWindow (mXDisplay,
                                    mXRoot,
                                    -100, -100, 1, 1,
                                    0,
                                    CopyFromParent,
                                    CopyFromParent,
                                    (Visual *)CopyFromParent,
                                    CWOverrideRedirect | CWEventMask,
                                    &attrs);

    char *atom_name;
    Atom atom;

    atom_name = g_strdup_printf ("_NET_WM_CM_S%d", 0);
    atom = XInternAtom (mXDisplay, atom_name, FALSE);
    g_free (atom_name);

    XSetSelectionOwner (mXDisplay, atom, mManagerWindow, CurrentTime);
    mIsWindowManager = XGetSelectionOwner (mXDisplay, atom) == mManagerWindow;

    if (!mIsWindowManager) {
        ERROR ("Couldn't acquire window manager selection");
    }

    atom_name = g_strdup_printf ("WM_S%d", 0);
    atom = XInternAtom (mXDisplay, atom_name, FALSE);
    g_free (atom_name);

    XSetSelectionOwner (mXDisplay, atom, mManagerWindow, CurrentTime);
    mIsCompositor = XGetSelectionOwner (mXDisplay, atom) == mManagerWindow;
    
    if (!mIsCompositor) {
        ERROR ("Couldn't acquire compositor selection");
    }

    return TRUE;
}


bool
compzillaControl::InitOutputWindow ()
{
    // Create the overlay window, and make the X server stop drawing windows
    mOverlay = XCompositeGetOverlayWindow (mXDisplay, mXRoot);
    if (!mOverlay) {
        ERROR ("failed call to XCompositeGetOverlayWindow\n");
        return false;
    }

    mMainwin = GetNativeWindow (mDOMWindow);
    if (!mMainwin) {
        ERROR ("failed to get native window\n");
        return false;
    }
    gdk_window_ref (mMainwin);
    gdk_window_set_override_redirect(mMainwin, true);

    // Put the our window into the overlay
    XReparentWindow (mXDisplay, GDK_DRAWABLE_XID (mMainwin), mOverlay, 0, 0);

    // FIXME: This causes BadMatch errors, because we aren't mapped yet.
    //XSetInputFocus (mXDisplay, GDK_DRAWABLE_XID (mMainwin), RevertToPointerRoot, CurrentTime);

    ShowOutputWindow ();

    return true;
}


void 
compzillaControl::ShowOutputWindow()
{
    // NOTE: Ripped off from compiz.  Not sure why this is needed.  Maybe to
    //       support multiple monitors with different dimensions?

    // mMainwin isn't mapped yet
    /*
    XserverRegion region = XFixesCreateRegionFromWindow (mXDisplay,
                                                         GDK_DRAWABLE_XID (mMainwin),
                                                         WindowRegionBounding);
    */

#if HAVE_XEVIE
    XserverRegion empty = XFixesCreateRegion (mXDisplay, NULL, 0);

    XFixesSetWindowShapeRegion (mXDisplay,
                                mOverlay,
                                ShapeBounding,
                                0, 0, 
                                0);

    XFixesSetWindowShapeRegion (mXDisplay,
                                mOverlay,
                                ShapeInput,
                                0, 0, 
                                empty);

    XFixesDestroyRegion (mXDisplay, empty);
#else 
    XRectangle rect = { 0, 0, DisplayWidth (mXDisplay, 0), DisplayHeight (mXDisplay, 0) };
    XserverRegion region = XFixesCreateRegion (mXDisplay, &rect, 1);

    XFixesSetWindowShapeRegion (mXDisplay,
                                mOverlay,
                                ShapeBounding,
                                0, 0, 
                                region);

    XFixesSetWindowShapeRegion (mXDisplay,
                                mOverlay,
                                ShapeInput,
                                0, 0, 
                                region);

    XFixesDestroyRegion (mXDisplay, region);
#endif
}


void 
compzillaControl::HideOutputWindow()
{
    // NOTE: Ripped off from compiz.  Not sure why this is needed.  Maybe to
    //       support multiple monitors with different dimensions?

    XserverRegion region = XFixesCreateRegion (mXDisplay, NULL, 0);

    XFixesSetWindowShapeRegion (mXDisplay,
                                mOverlay,
                                ShapeBounding,
                                0, 0, 
                                region);

    XFixesDestroyRegion (mXDisplay, region);
}


#ifdef OVERLAY_INPUT_REGION
XserverRegion 
compzillaControl::ConvertNsRegion (nsRegion &region)
{
    XRectangle rects [region.GetNumRects ()];
    int i = 0;

    nsRegionRectIterator iter = nsRegionRectIterator (region);
    while (const nsRect *rect = iter.Next()) {
        rects[i].x = rect->x;
        rects[i].y = rect->y;
        rects[i].width = rect->width;
        rects[i].height = rect->height;
        i++;
    }

    return XFixesCreateRegion (mXDisplay, rects, i);
}


void
compzillaControl::SetOverlayInput (compzillaWindow *win)
{
    nsRegion region;

    if (win) {
        win->GetDisplayRegion (region);
    }

    XserverRegion xregion = ConvertNsRegion (region);

    XFixesSetWindowShapeRegion (mXDisplay,
                                mOverlay,
                                ShapeInput,
                                0, 0, 
                                xregion);
}
#endif /* OVERLAY_INPUT_REGION */


int 
compzillaControl::ErrorHandler (Display *dpy, XErrorEvent *err)
{
    sErrorCnt++;

    char str[128];
    char *name = 0;
    int  o;

    XGetErrorDatabaseText (dpy, "XlibMessage", "XError", "", str, 128);
    ERROR ("%s", str);

    o = err->error_code - damage_error;
    switch (o) {
    case BadDamage:
	name = "BadDamage";
	break;
    default:
	break;
    }

    if (name) {
        ERROR (": %s\n  ", name);
    } else {
	XGetErrorText (dpy, err->error_code, str, 128);
	ERROR (": %s\n  ", str);
    }

    XGetErrorDatabaseText (dpy, "XlibMessage", "ResourceID", "%d", str, 128);
    ERROR (str, err->resourceid);
    ERROR ("\n  ");

    XGetErrorDatabaseText (dpy, "XlibMessage", "MajorCode", "%d", str, 128);
    ERROR (str, err->request_code);

    sprintf (str, "%d", err->request_code);
    XGetErrorDatabaseText (dpy, "XRequest", str, "", str, 128);
    if (strcmp (str, "") != 0) {
	ERROR (" (%s)", str);
    }
    ERROR ("\n  ");

    XGetErrorDatabaseText (dpy, "XlibMessage", "MinorCode", "%d", str, 128);
    ERROR (str, err->minor_code);
    ERROR ("\n");

    return 0;
}


int 
compzillaControl::ClearErrors (Display *dpy)
{
    XSync (dpy, FALSE);

    int lastcnt = sErrorCnt;
    sErrorCnt = 0;
    return lastcnt;
}


void
compzillaControl::AddWindow (Window win)
{
    INFO ("AddWindow for window %p\n", win);

    nsRefPtr<compzillaWindow> compwin;
    if (NS_OK != CZ_NewCompzillaWindow (mXDisplay, win, getter_AddRefs (compwin)))
        return;

    if (compwin->mAttr.c_class == InputOnly) {
        WARNING ("AddWindow ignoring InputOnly window %p\n", win);
        return;
    }

    mWindowMap.Put (win, compwin);
    
    compzillaIWindow *iwin = compwin;

    for (PRUint32 i = mObservers.Count() - 1; i != PRUint32(-1); --i) {
        nsCOMPtr<compzillaIControlObserver> observer = mObservers.ObjectAt(i);
        observer->WindowCreate (iwin);
    }
}


void
compzillaControl::DestroyWindow (Window win)
{
    nsRefPtr<compzillaWindow> compwin = FindWindow (win);
    if (compwin) {
        compwin->DestroyWindow ();

        compzillaIWindow *iwin = compwin;

        for (PRUint32 i = mObservers.Count() - 1; i != PRUint32(-1); --i) {
            nsCOMPtr<compzillaIControlObserver> observer = mObservers.ObjectAt(i);
            observer->WindowDestroy (iwin);
        }

        SPEW ("BEFORE REMOVING %p\n", compwin.get());
        mWindowMap.Remove (win);
        SPEW ("AFTER REMOVING %p\n", compwin.get());
    }
}


void
compzillaControl::ForgetWindow (Window win)
{
    nsRefPtr<compzillaWindow> compwin = FindWindow (win);
    if (compwin) {
        compwin->DestroyWindow ();
        mWindowMap.Remove (win);
    }
}


void
compzillaControl::MapWindow (Window win, bool override_redirect)
{
    nsRefPtr<compzillaWindow> compwin = FindWindow (win);
    if (compwin) {
        compwin->MapWindow (override_redirect);
    }
}


void
compzillaControl::UnmapWindow (Window win)
{
    nsRefPtr<compzillaWindow> compwin = FindWindow (win);
    if (compwin) {
        compwin->UnmapWindow ();
    }
}


void 
compzillaControl::WindowConfigured (bool isNotify,
                                    Window win,
                                    PRInt32 x, PRInt32 y,
                                    PRInt32 width, PRInt32 height,
                                    PRInt32 border,
                                    Window aboveWin,
                                    bool override_redirect)
{
    nsRefPtr<compzillaWindow> compwin = FindWindow (win);
    if (compwin) {
        nsRefPtr<compzillaWindow> aboveCompWin = FindWindow (aboveWin);

        compwin->WindowConfigured (isNotify,
                                   x, y,
                                   width, height,
                                   border,
                                   aboveCompWin,
                                   override_redirect);
    } else if (!isNotify) {
        // Window we are not monitoring, so send the configure.
        ConfigureWindow (win, x, y, width, height, border);
    }
}


void
compzillaControl::PropertyChanged (Window win, Atom prop, bool deleted)
{
    nsRefPtr<compzillaWindow> compwin = FindWindow (win);
    if (compwin) {
        compwin->PropertyChanged (prop, deleted);
    }
}


void
compzillaControl::WindowDamaged (Window win, XRectangle *rect)
{
    nsRefPtr<compzillaWindow> compwin = FindWindow (win);
    if (compwin) {
        compwin->WindowDamaged (rect);
    }
}


void
compzillaControl::ClientMessaged (Window win,
                                  Atom type, int format, long *data/*[5]*/)
{
    nsRefPtr<compzillaWindow> compwin = FindWindow (win);
    if (compwin) {
        compwin->ClientMessaged (type, format, data);
    }
    else {
        for (PRUint32 i = mObservers.Count() - 1; i != PRUint32(-1); --i) {
            nsCOMPtr<compzillaIControlObserver> observer = mObservers.ObjectAt(i);
            observer->RootClientMessageRecv ((long)type,
                                             format,
                                             data[0],
                                             data[1],
                                             data[2],
                                             data[3],
                                             data[4]);
        }
    }
}

already_AddRefed<compzillaWindow>
compzillaControl::FindWindow (Window win)
{
    compzillaWindow *compwin;
    mWindowMap.Get(win, &compwin);
    if (compwin && compwin->mIsDestroyed) {
        return NULL;
    }
    return compwin;
}


GdkFilterReturn
compzillaControl::Filter (GdkXEvent *xevent, GdkEvent *event)
{
    XEvent *x11_event = (XEvent*) xevent;

    switch (x11_event->type) {
    case ClientMessage: {
        SPEW ("ClientMessage: window=0x%0x, type=%s, format=%d\n",
              x11_event->xclient.window,
              XGetAtomName (mXDisplay, x11_event->xclient.message_type),
              x11_event->xclient.format);

        ClientMessaged (x11_event->xclient.window,
                        x11_event->xclient.message_type,
                        x11_event->xclient.format,
                        x11_event->xclient.data.l);
        break;
    }
    case CreateNotify: {
        SPEW ("CreateNotify: window=0x%0x, parent=%p, x=%d, y=%d, width=%d, height=%d, "
              "override=%d\n",
               x11_event->xcreatewindow.window,
               x11_event->xcreatewindow.parent,
               x11_event->xcreatewindow.x,
               x11_event->xcreatewindow.y,
               x11_event->xcreatewindow.width,
               x11_event->xcreatewindow.height,
               x11_event->xcreatewindow.override_redirect);

        if (x11_event->xcreatewindow.window == GDK_DRAWABLE_XID (this->mMainwin)) {
            WARNING ("CreateNotify: discarding event on mainwin\n");
            return GDK_FILTER_REMOVE;
        }

        if (x11_event->xcreatewindow.parent == mXRoot) {
            AddWindow (x11_event->xcreatewindow.window);
        }
        break;
    }
    case DestroyNotify: {
        SPEW ("DestroyNotify: window=0x%0x\n", x11_event->xdestroywindow.window);

        DestroyWindow (x11_event->xdestroywindow.window);
        break;
    }
    case ConfigureNotify: {
        SPEW ("ConfigureNotify: window=%p, x=%d, y=%d, width=%d, height=%d, "
              "border=%d, override=%d\n",
              x11_event->xconfigure.window,
              x11_event->xconfigure.x,
              x11_event->xconfigure.y,
              x11_event->xconfigure.width,
              x11_event->xconfigure.height,
              x11_event->xconfigure.border_width,
              x11_event->xconfigure.override_redirect);

        // This is driven by compzilla or from an override_redirect itself.
        WindowConfigured (true,
                          x11_event->xconfigure.window,
                          x11_event->xconfigure.x,
                          x11_event->xconfigure.y,
                          x11_event->xconfigure.width,
                          x11_event->xconfigure.height,
                          x11_event->xconfigure.border_width,
                          x11_event->xconfigure.above,
                          x11_event->xconfigure.override_redirect);
        break;
    }
    case ConfigureRequest: {
        SPEW ("ConfigureRequest: window=%p, x=%d, y=%d, width=%d, height=%d, "
              "border=%d\n",
              x11_event->xconfigurerequest.window,
              x11_event->xconfigurerequest.x,
              x11_event->xconfigurerequest.y,
              x11_event->xconfigurerequest.width,
              x11_event->xconfigurerequest.height,
              x11_event->xconfigurerequest.border_width);

        if (x11_event->xconfigurerequest.parent == mXRoot) {
            // This is driven by the X app, not compzilla.
            WindowConfigured (false,
                              x11_event->xconfigurerequest.window,
                              x11_event->xconfigurerequest.x,
                              x11_event->xconfigurerequest.y,
                              x11_event->xconfigurerequest.width,
                              x11_event->xconfigurerequest.height,
                              x11_event->xconfigurerequest.border_width,
                              x11_event->xconfigurerequest.above,
                              false);
        }
        break;
    }
    case ReparentNotify: {
        SPEW ("ReparentNotify: window=%p, parent=%p, x=%d, y=%d, override_redirect=%d\n",
              x11_event->xreparent.window,
              x11_event->xreparent.parent,
              x11_event->xreparent.x,
              x11_event->xreparent.y,
              x11_event->xreparent.override_redirect);

        if (x11_event->xreparent.window == GDK_DRAWABLE_XID (this->mMainwin)) {
            // Keep the new parent so we can ignore events on it.
            mMainwinParent = x11_event->xreparent.parent;

            // Let the mainwin draw normally by unredirecting our new parent.
            ForgetWindow (x11_event->xreparent.parent);

            WARNING ("ReparentNotify: discarding event on mainwin\n");
            return GDK_FILTER_REMOVE;
        }

        if (x11_event->xreparent.window == mMainwinParent) {
            WARNING ("ReparentNotify: discarding event on mainwin's parent\n");
            return GDK_FILTER_REMOVE;
        }

        nsRefPtr<compzillaWindow> compwin = FindWindow (x11_event->xreparent.window);

        if (x11_event->xreparent.parent == mXRoot && !compwin) {
            AddWindow (x11_event->xreparent.window);
        } else if (compwin) {
            DestroyWindow (x11_event->xreparent.window);
        }

        break;
    }
    case MapRequest:
        if (x11_event->xmaprequest.parent == mXRoot) {
            XMapRaised (mXDisplay, x11_event->xmaprequest.window);
        }
        break;
    case MapNotify: {
        SPEW ("MapNotify: window=0x%0x, override=%d\n", x11_event->xmap.window, 
              x11_event->xmap.override_redirect);

        MapWindow (x11_event->xmap.window, x11_event->xmap.override_redirect);
        break;
    }
    case UnmapNotify: {
        SPEW ("UnmapNotify: window=0x%0x\n", x11_event->xunmap.window);

        UnmapWindow (x11_event->xunmap.window);
        break;
    }
    case PropertyNotify: {
        SPEW ("PropertyChange: window=0x%0x, atom=%s\n", 
              x11_event->xproperty.window, 
              XGetAtomName(x11_event->xany.display, x11_event->xproperty.atom));
        PropertyChanged (x11_event->xproperty.window, 
                         x11_event->xproperty.atom, 
                         x11_event->xproperty.state == PropertyDelete);
        break;
    }
    case _KeyPress:
    case KeyRelease:
    case ButtonPress:
    case ButtonRelease:
    case MotionNotify: {
        switch (x11_event->type) {
        case _KeyPress:
            SPEW ("KeyPress: window=0x%0x, state=%d, keycode=%d\n", 
                  x11_event->xkey.window, x11_event->xkey.state, x11_event->xkey.keycode);
            break;
        case KeyRelease:
            SPEW ("KeyRelease: window=0x%0x, state=%d, keycode=%d\n", 
                  x11_event->xkey.window, x11_event->xkey.state, x11_event->xkey.keycode);
            break;
        case ButtonPress:
            SPEW ("ButtonPress: window=%p, x=%d, y=%d, x_root=%d, y_root=%d, button=%d\n", 
                  x11_event->xbutton.window, x11_event->xbutton.x, x11_event->xbutton.y,
                  x11_event->xbutton.x_root, x11_event->xbutton.y_root,
                  x11_event->xbutton.button);
            break;
        case ButtonRelease:
            SPEW ("ButtonRelease: window=0x%0x, x=%d, y=%d, button=%d\n", 
                  x11_event->xbutton.window, x11_event->xbutton.x, x11_event->xbutton.y,
                  x11_event->xbutton.button);
            break;
        case MotionNotify:
            SPEW ("MotionNotify: window=0x%0x, x=%d, y=%d, state=%d\n", 
                  x11_event->xmotion.window, x11_event->xmotion.x, x11_event->xmotion.y, 
                  x11_event->xmotion.state);
            break;
        }

#if HAVE_XEVIE
        // Redirect all mouse/kayboard events to our mozilla window.
        /*
        Window mainwin = GDK_WINDOW_XID (mMainwin);
        if (x11_event->xany.window != mainwin) {
            x11_event->xany.window = mainwin;
            XevieSendEvent (mXDisplay, x11_event, XEVIE_MODIFIED);
        } else {
            XevieSendEvent (mXDisplay, x11_event, XEVIE_UNMODIFIED);
        }
        */
        XevieSendEvent (mXDisplay, x11_event, XEVIE_UNMODIFIED);

        nsCOMPtr<nsIWidget> widget;
        if (NS_OK == GetNativeWidget (mDOMWindow, getter_AddRefs (widget))) {
            // Call nsIWidget::DispatchEvent?
        }

        // Don't let Gtk process the original event.
        return GDK_FILTER_REMOVE;
#endif

        break;
    }
    case Expose:
        ERROR ("Expose event win=%p\n", x11_event->xexpose.window);
        break;
    default:
        if (x11_event->type == damage_event + XDamageNotify) {
            XDamageNotifyEvent *damage_ev = (XDamageNotifyEvent *) x11_event;

#if 0
            SPEW ("DAMAGE: drawable=%p, x=%d, y=%d, width=%d, height=%d\n", 
                  damage_ev->drawable, damage_ev->area.x, damage_ev->area.y, 
                  damage_ev->area.width, damage_ev->area.height);
#endif

            int cnt = 0;
            do {
                WindowDamaged (damage_ev->drawable, &damage_ev->area);
                cnt++;
            } while (XCheckTypedEvent (mXDisplay, 
                                       damage_event + XDamageNotify,
                                       x11_event));

            if (cnt > 1) {
                SPEW("DAMAGE: Handled %d pending events!\n", cnt);
            }

            return GDK_FILTER_REMOVE;
        }
        else if (x11_event->type == shape_event + ShapeNotify) {
            NS_NOTYETIMPLEMENTED ("ShapeNotify event");
        }
        else {
            ERROR ("Unhandled window event %d\n", x11_event->type);
        }
        break;
    }

    return GDK_FILTER_CONTINUE;
}
