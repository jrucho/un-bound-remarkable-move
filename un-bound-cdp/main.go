// chromium-cdp: drive the on-device headless Chromium over the DevTools
// protocol for the Chromium e-ink browser.
//
// One process does everything the takeover session needs:
//   - waits for the browser's debug port to come up
//   - navigates to the URL and waits for *real* content (load event, then an
//     optional CSS selector, then a paint settle) — fixes the SPA-spinner
//     problem of one-shot --screenshot
//   - captures the page to JPEG and atomically renames it over -out
//     (the viewer watches that file and repaints on change)
//   - reads "TAP x y" / "SWIPE x1 y1 x2 y2" lines from -fifo and replays
//     them as CDP mouse events immediately; a capture worker publishes the
//     page's reaction without ever blocking input replay
//   - replays the viewer's on-screen keyboard ("KEY <cp> <mods>" /
//     "KEYN <name> <mods>") as CDP key events, and resizes the viewport
//     ("VP <w> <h>") so the page reflows above the keyboard band
//   - optionally recaptures every -refresh, so streaming pages (a ChatGPT
//     answer being written) animate onto the e-ink
//
// Exits on SIGTERM/SIGINT, or when the browser goes away.
package main

import (
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"math"
	"net/http"
	neturl "net/url"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/gorilla/websocket"
)

// ---------- tiny CDP client ----------

type cdpClient struct {
	conn    *websocket.Conn
	writeMu sync.Mutex
	nextID  atomic.Int64
	pending map[int64]chan cdpResponse
	pendMu  sync.Mutex
	events  chan cdpEvent
	dead    chan struct{}
	deadErr error
}

type cdpResponse struct {
	Result json.RawMessage
	Err    error
}

type cdpEvent struct {
	Method string
	Params json.RawMessage
}

type cdpMessage struct {
	ID     int64           `json:"id,omitempty"`
	Method string          `json:"method,omitempty"`
	Params json.RawMessage `json:"params,omitempty"`
	Result json.RawMessage `json:"result,omitempty"`
	Error  *struct {
		Code    int    `json:"code"`
		Message string `json:"message"`
	} `json:"error,omitempty"`
}

func dialPage(wsURL string) (*cdpClient, error) {
	d := websocket.Dialer{HandshakeTimeout: 10 * time.Second}
	// CDP screenshots of a 1620x2160 page run ~1-2 MB base64; default frame
	// limit is fine but be generous.
	d.ReadBufferSize = 1 << 20
	conn, _, err := d.Dial(wsURL, nil)
	if err != nil {
		return nil, err
	}
	c := &cdpClient{
		conn:    conn,
		pending: map[int64]chan cdpResponse{},
		events:  make(chan cdpEvent, 256),
		dead:    make(chan struct{}),
	}
	go c.readLoop()
	return c, nil
}

func (c *cdpClient) readLoop() {
	for {
		_, data, err := c.conn.ReadMessage()
		if err != nil {
			c.deadErr = err
			close(c.dead)
			// fail all pending calls
			c.pendMu.Lock()
			for id, ch := range c.pending {
				ch <- cdpResponse{Err: fmt.Errorf("connection closed: %w", err)}
				delete(c.pending, id)
			}
			c.pendMu.Unlock()
			return
		}
		var msg cdpMessage
		if err := json.Unmarshal(data, &msg); err != nil {
			continue
		}
		if msg.ID != 0 {
			c.pendMu.Lock()
			ch := c.pending[msg.ID]
			delete(c.pending, msg.ID)
			c.pendMu.Unlock()
			if ch != nil {
				resp := cdpResponse{Result: msg.Result}
				if msg.Error != nil {
					resp.Err = fmt.Errorf("cdp: %s (%d)", msg.Error.Message, msg.Error.Code)
				}
				ch <- resp
			}
		} else if msg.Method != "" {
			select {
			case c.events <- cdpEvent{Method: msg.Method, Params: msg.Params}:
			default: // drop events rather than block the reader
			}
		}
	}
}

func (c *cdpClient) call(method string, params any, out any) error {
	return c.callT(method, params, out, 60*time.Second)
}

func (c *cdpClient) callT(method string, params any, out any, timeout time.Duration) error {
	id := c.nextID.Add(1)
	var raw json.RawMessage
	if params != nil {
		b, err := json.Marshal(params)
		if err != nil {
			return err
		}
		raw = b
	}
	b, err := json.Marshal(cdpMessage{ID: id, Method: method, Params: raw})
	if err != nil {
		return err
	}
	ch := make(chan cdpResponse, 1)
	c.pendMu.Lock()
	c.pending[id] = ch
	c.pendMu.Unlock()

	c.writeMu.Lock()
	err = c.conn.WriteMessage(websocket.TextMessage, b)
	c.writeMu.Unlock()
	if err != nil {
		c.pendMu.Lock()
		delete(c.pending, id)
		c.pendMu.Unlock()
		return err
	}
	select {
	case resp := <-ch:
		if resp.Err != nil {
			return resp.Err
		}
		if out != nil {
			return json.Unmarshal(resp.Result, out)
		}
		return nil
	case <-time.After(timeout):
		c.pendMu.Lock()
		delete(c.pending, id)
		c.pendMu.Unlock()
		return fmt.Errorf("cdp: %s timed out", method)
	case <-c.dead:
		return fmt.Errorf("cdp: connection closed during %s", method)
	}
}

// evaluate runs a JS expression and unmarshals its by-value result into out.
// Polling callers get a short timeout so one wedged evaluate can't stall the
// session for a minute.
func (c *cdpClient) evaluate(expr string, awaitPromise bool, out any) error {
	return c.evaluateT(expr, awaitPromise, out, 60*time.Second)
}

func (c *cdpClient) evaluateT(expr string, awaitPromise bool, out any, timeout time.Duration) error {
	var res struct {
		Result struct {
			Value json.RawMessage `json:"value"`
		} `json:"result"`
		ExceptionDetails *struct {
			Text string `json:"text"`
		} `json:"exceptionDetails"`
	}
	err := c.callT("Runtime.evaluate", map[string]any{
		"expression":    expr,
		"returnByValue": true,
		"awaitPromise":  awaitPromise,
	}, &res, timeout)
	if err != nil {
		return err
	}
	if res.ExceptionDetails != nil {
		return fmt.Errorf("js exception: %s", res.ExceptionDetails.Text)
	}
	if out != nil && res.Result.Value != nil {
		return json.Unmarshal(res.Result.Value, out)
	}
	return nil
}

// ---------- target discovery ----------

type targetInfo struct {
	ID    string `json:"id"`
	Type  string `json:"type"`
	URL   string `json:"url"`
	WsURL string `json:"webSocketDebuggerUrl"`
}

func pageTargets(port int) ([]targetInfo, error) {
	resp, err := http.Get(fmt.Sprintf("http://127.0.0.1:%d/json/list", port))
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, err
	}
	var targets []targetInfo
	if err := json.Unmarshal(body, &targets); err != nil {
		return nil, err
	}
	return targets, nil
}

// pageTarget waits for the debug port and returns the first page target.
func pageTarget(port int, wait time.Duration) (targetInfo, error) {
	deadline := time.Now().Add(wait)
	var lastErr error
	for time.Now().Before(deadline) {
		targets, err := pageTargets(port)
		if err == nil {
			for _, t := range targets {
				if t.Type == "page" && t.WsURL != "" {
					return t, nil
				}
			}
			lastErr = fmt.Errorf("no page target among %d targets", len(targets))
		} else {
			lastErr = err
		}
		time.Sleep(300 * time.Millisecond)
	}
	return targetInfo{}, fmt.Errorf("debug port %d never became ready: %v", port, lastErr)
}

// ---------- rendering ----------

type driver struct {
	sessionMu    sync.RWMutex
	c            *cdpClient
	activeID     string
	rootID       string
	root         *cdpClient
	dpr          float64
	cssWidth     int
	cssHeight    int
	targetGen    atomic.Int64
	captureMu    sync.Mutex
	outPath      string
	lastHash     [32]byte
	haveHash     bool
	lastSize     int64
	lastTargetID string
	// Set around NAV's navigate+waitContent: concurrent burst captures must
	// not publish a committing/half-loaded document onto the glass. navGen
	// closes the straddle window: a capture RPC that began before a nav
	// started must not publish after it finishes.
	navigating atomic.Bool
	navGen     atomic.Int64

	// Tier-1 "paper" adapters: hostname suffix -> CSS (+ optional JS strip).
	// Injected after content settles so a page reads like print (see
	// paperify/adapters/).
	adapters map[string]adapter
}

func (d *driver) session() (*cdpClient, string, int64) {
	d.sessionMu.RLock()
	defer d.sessionMu.RUnlock()
	return d.c, d.activeID, d.targetGen.Load()
}

func (d *driver) applyViewport(c *cdpClient) error {
	d.sessionMu.RLock()
	w, h, dpr := d.cssWidth, d.cssHeight, d.dpr
	d.sessionMu.RUnlock()
	return c.call("Emulation.setDeviceMetricsOverride", map[string]any{
		"width": w, "height": h, "deviceScaleFactor": dpr, "mobile": true,
	}, nil)
}

func (d *driver) switchTo(id string, c *cdpClient) error {
	_ = c.call("Page.enable", nil, nil)
	_ = c.call("Runtime.enable", nil, nil)
	if err := d.applyViewport(c); err != nil {
		return err
	}
	_ = c.call("Page.bringToFront", nil, nil)

	d.captureMu.Lock()
	d.sessionMu.Lock()
	old := d.c
	oldID := d.activeID
	d.c = c
	d.activeID = id
	d.targetGen.Add(1)
	d.haveHash = false
	d.sessionMu.Unlock()
	d.captureMu.Unlock()
	log.Printf("target %s -> %s", oldID, id)
	if old != nil && old != d.root && old != c {
		_ = old.conn.Close()
	}
	return nil
}

func (d *driver) cssCoord(px int) float64 {
	return float64(px) / d.dpr
}

func (d *driver) cssSize(px int) int {
	return max(1, int(math.Round(float64(px)/d.dpr)))
}

func (d *driver) setViewport(physicalWidth, physicalHeight int) error {
	d.sessionMu.Lock()
	d.cssWidth = d.cssSize(physicalWidth)
	d.cssHeight = d.cssSize(physicalHeight)
	c := d.c
	d.sessionMu.Unlock()
	return d.applyViewport(c)
}

type adapter struct {
	css string
	js  string // optional pre-CSS strip: force light mode, remove nodes
}

// loadAdapters reads <dir>/<host>.css (and optional <host>.js), keyed by the
// base filename as a hostname substring match: grokipedia.css applies to any
// host containing "grokipedia".
func loadAdapters(dir string) map[string]adapter {
	out := map[string]adapter{}
	if dir == "" {
		return out
	}
	ents, err := os.ReadDir(dir)
	if err != nil {
		log.Printf("adapters: %v", err)
		return out
	}
	for _, e := range ents {
		name := e.Name()
		if e.IsDir() || !strings.HasSuffix(name, ".css") || strings.HasPrefix(name, "_") {
			continue
		}
		b, err := os.ReadFile(filepath.Join(dir, name))
		if err != nil {
			continue
		}
		key := strings.TrimSuffix(name, ".css")
		a := adapter{css: string(b)}
		if jb, err := os.ReadFile(filepath.Join(dir, key+".js")); err == nil {
			a.js = string(jb)
		}
		out[key] = a
		log.Printf("adapter loaded: %s (css %d B, js %d B)", key, len(a.css), len(a.js))
	}
	return out
}

// applyAdapter runs the matching host's optional JS strip, then injects its
// stylesheet into a <style id="__paperify"> (idempotent: re-setting
// textContent updates in place). Safe to call on every settle, including SPA
// same-doc navs, so a route change inside one site keeps the treatment.
func (d *driver) applyAdapter() {
	if len(d.adapters) == 0 {
		return
	}
	c, _, _ := d.session()
	var host string
	if err := c.evaluateT("location.hostname", false, &host, 5*time.Second); err != nil || host == "" {
		return
	}
	var a adapter
	found := false
	for key, cand := range d.adapters {
		if strings.Contains(host, key) {
			a = cand
			found = true
			break
		}
	}
	if !found {
		// No adapter for this host: remove any stale one from a prior page.
		_ = c.evaluateT("(()=>{const s=document.getElementById('__paperify');if(s)s.remove();return 1})()", false, nil, 5*time.Second)
		return
	}
	// JS strip first (light mode, node removal) so the CSS lands on the
	// intended DOM. Wrapped so an adapter error can't wedge the session.
	if a.js != "" {
		jsLit, _ := json.Marshal(a.js)
		wrap := fmt.Sprintf("(()=>{try{(0,eval)(%s)}catch(e){}return 1})()", string(jsLit))
		_ = c.evaluateT(wrap, false, nil, 5*time.Second)
	}
	// Encode the CSS as a JSON string literal so any bytes survive the eval.
	lit, _ := json.Marshal(a.css)
	expr := fmt.Sprintf("(()=>{let s=document.getElementById('__paperify');"+
		"if(!s){s=document.createElement('style');s.id='__paperify';"+
		"document.documentElement.appendChild(s);}s.textContent=%s;return 1})()", string(lit))
	if err := c.evaluateT(expr, false, nil, 5*time.Second); err != nil {
		log.Printf("adapter inject: %v", err)
	} else {
		log.Printf("adapter applied to %s", host)
	}
}

// capture screenshots the viewport and atomically publishes it to outPath if
// the pixels changed. Returns whether a new image was published.
func (d *driver) capture() (bool, error) {
	d.captureMu.Lock()
	defer d.captureMu.Unlock()
	c, targetID, targetGen := d.session()
	gen := d.navGen.Load()
	var res struct {
		Data string `json:"data"`
	}
	// JPEG, not PNG: Chromium's PNG encode of a 1620x2160 frame costs seconds
	// of A53 CPU; JPEG is several times cheaper and the e-ink quantization
	// hides the artifacts. The viewer's QImage sniffs the format from the
	// bytes, so the .png path name stays honest enough.
	if err := c.call("Page.captureScreenshot", map[string]any{
		"format": "jpeg", "quality": 80,
	}, &res); err != nil {
		return false, err
	}
	img, err := base64.StdEncoding.DecodeString(res.Data)
	if err != nil {
		return false, err
	}
	h := sha256.Sum256(img)
	if st, err := os.Stat(d.outPath); err != nil || st.Size() != d.lastSize {
		d.haveHash = false // file changed under us; force a republish
	}
	if d.haveHash && d.lastTargetID == targetID && h == d.lastHash {
		return false, nil
	}
	if d.navigating.Load() || d.navGen.Load() != gen || d.targetGen.Load() != targetGen {
		return false, nil // mid-NAV frame; the post-nav capture publishes instead
	}
	tmp := d.outPath + ".tmp"
	if err := os.WriteFile(tmp, img, 0644); err != nil {
		return false, err
	}
	if err := os.Rename(tmp, d.outPath); err != nil {
		return false, err
	}
	d.lastHash = h
	d.haveHash = true
	d.lastSize = int64(len(img))
	d.lastTargetID = targetID
	return true, nil
}

// waitContent implements the anti-spinner wait: document complete, optional
// selector, then a double-rAF paint settle.
func (d *driver) waitContent(selector string, timeout time.Duration) {
	c, _, _ := d.session()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		var oldDoc bool
		if err := c.evaluateT("!!window.__chromium_app_pre_nav", false, &oldDoc, 5*time.Second); err == nil && !oldDoc {
			break
		}
		time.Sleep(150 * time.Millisecond)
	}
	for time.Now().Before(deadline) {
		var state string
		if err := c.evaluateT("document.readyState", false, &state, 5*time.Second); err == nil && state == "complete" {
			break
		}
		time.Sleep(250 * time.Millisecond)
	}
	if selector != "" {
		expr := fmt.Sprintf("!!document.querySelector(%q)", selector)
		for time.Now().Before(deadline) {
			var found bool
			if err := c.evaluateT(expr, false, &found, 5*time.Second); err == nil && found {
				log.Printf("selector matched: %s", selector)
				break
			}
			time.Sleep(300 * time.Millisecond)
		}
	}
	// Two animation frames = the page has actually painted what it has.
	_ = c.evaluateT("new Promise(r=>requestAnimationFrame(()=>requestAnimationFrame(()=>r(1))))", true, nil, 10*time.Second)
	time.Sleep(500 * time.Millisecond) // last-moment layout/images
	d.applyAdapter()                   // reskin as paper before the first capture
}

// navigate starts a navigation; sameDoc reports a same-document navigation
// (fragment jump / pushState route) — the document survives those, so there
// is no load to wait for and waitContent would poll its pre-nav marker until
// the full deadline with the glass frozen.
func (d *driver) navigate(url string) (sameDoc bool, err error) {
	c, _, _ := d.session()
	// Mark the old document: waitContent must not trust readyState/selector
	// results until this marker is gone (i.e. the new document committed).
	// Short timeout — a wedged old page must not hold the navigation hostage.
	_ = c.evaluateT("window.__chromium_app_pre_nav = 1", false, nil, 5*time.Second)
	var res struct {
		ErrorText string `json:"errorText"`
		LoaderID  string `json:"loaderId"`
	}
	if err := c.call("Page.navigate", map[string]any{"url": url}, &res); err != nil {
		return false, err
	}
	if res.ErrorText != "" {
		return false, fmt.Errorf("navigation failed: %s", res.ErrorText)
	}
	if res.LoaderID == "" {
		// Same-document: the document (and our marker) survives — clean the
		// marker up so nothing ever waits on it.
		_ = c.evaluateT("delete window.__chromium_app_pre_nav", false, nil, 5*time.Second)
		return true, nil
	}
	return false, nil
}

// ---------- input ----------

func (d *driver) tap(x, y int) {
	c, _, _ := d.session()
	base := map[string]any{
		"x": d.cssCoord(x), "y": d.cssCoord(y), "button": "left", "clickCount": 1,
	}
	press := map[string]any{"type": "mousePressed", "buttons": 1}
	release := map[string]any{"type": "mouseReleased", "buttons": 0}
	for k, v := range base {
		press[k] = v
		release[k] = v
	}
	if err := c.call("Input.dispatchMouseEvent", press, nil); err != nil {
		log.Printf("tap press: %v", err)
		return
	}
	time.Sleep(40 * time.Millisecond)
	if err := c.call("Input.dispatchMouseEvent", release, nil); err != nil {
		log.Printf("tap release: %v", err)
	}
}

// ---------- keyboard ----------

// The viewer's on-screen keyboard sends "KEY <codepoint> <mods>" for
// printable characters and "KEYN <name> <mods>" for named keys; mods uses
// CDP's bits (1=Alt, 2=Ctrl, 8=Shift; shift is already baked into the
// codepoint and rides along for chorded named keys like shift+enter).
type namedKey struct {
	key  string
	code string
	text string
	vk   int
}

var namedKeys = map[string]namedKey{
	"enter":     {"Enter", "Enter", "\r", 13},
	"backspace": {"Backspace", "Backspace", "", 8},
	"tab":       {"Tab", "Tab", "\t", 9},
	"esc":       {"Escape", "Escape", "", 27},
	"del":       {"Delete", "Delete", "", 46},
	"up":        {"ArrowUp", "ArrowUp", "", 38},
	"down":      {"ArrowDown", "ArrowDown", "", 40},
	"left":      {"ArrowLeft", "ArrowLeft", "", 37},
	"right":     {"ArrowRight", "ArrowRight", "", 39},
	"pgup":      {"PageUp", "PageUp", "", 33},
	"pgdn":      {"PageDown", "PageDown", "", 34},
	"home":      {"Home", "Home", "", 36},
	"end":       {"End", "End", "", 35},
}

func charKey(r rune) namedKey {
	k := namedKey{key: string(r), text: string(r)}
	switch {
	case r >= 'a' && r <= 'z':
		k.vk = int(r) - 'a' + 'A'
		k.code = fmt.Sprintf("Key%c", r-'a'+'A')
	case r >= 'A' && r <= 'Z':
		k.vk = int(r)
		k.code = "Key" + string(r)
	case r >= '0' && r <= '9':
		k.vk = int(r)
		k.code = "Digit" + string(r)
	case r == ' ':
		k.vk = 32
		k.code = "Space"
	}
	return k
}

// sendKey replays one key as a down/up pair, puppeteer-style: keys carrying
// text insert it into the focused element; ctrl/alt chords go as raw key
// events (no insertion), the way a physical keyboard would.
func (d *driver) sendKey(k namedKey, mods int) {
	c, _, _ := d.session()
	text := k.text
	if mods&(1|2) != 0 {
		text = ""
	}
	downType := "keyDown"
	if text == "" {
		downType = "rawKeyDown"
	}
	down := map[string]any{
		"type": downType, "modifiers": mods,
		"key": k.key, "code": k.code,
		"windowsVirtualKeyCode": k.vk, "nativeVirtualKeyCode": k.vk,
	}
	if text != "" {
		down["text"] = text
		down["unmodifiedText"] = text
	}
	if err := c.call("Input.dispatchKeyEvent", down, nil); err != nil {
		// Still send the up: a timed-out down may have reached the page, and
		// an unmatched keydown leaves editors thinking the key is held.
		log.Printf("key down: %v", err)
	}
	err := c.call("Input.dispatchKeyEvent", map[string]any{
		"type": "keyUp", "modifiers": mods,
		"key": k.key, "code": k.code,
		"windowsVirtualKeyCode": k.vk, "nativeVirtualKeyCode": k.vk,
	}, nil)
	if err != nil {
		log.Printf("key up: %v", err)
	}
}

func (d *driver) swipe(x1, y1, x2, y2 int) {
	c, _, _ := d.session()
	// A swipe on paper means "move the page the way my finger moved":
	// finger up (y2 < y1) scrolls the page down → positive wheel deltaY.
	err := c.call("Input.dispatchMouseEvent", map[string]any{
		"type": "mouseWheel", "x": d.cssCoord(x1), "y": d.cssCoord(y1),
		"deltaX": float64(x1-x2) / d.dpr, "deltaY": float64(y1-y2) / d.dpr,
	}, nil)
	if err != nil {
		log.Printf("swipe: %v", err)
	}
}

// Firebase and similar OAuth libraries open authentication in a second page
// target. Headless Chromium creates it normally, but there is no browser
// chrome to display it, so follow the newest non-root page until it closes.
func monitorPopups(port int, d *driver, wake func()) {
	ticker := time.NewTicker(250 * time.Millisecond)
	defer ticker.Stop()
	for range ticker.C {
		targets, err := pageTargets(port)
		if err != nil {
			continue
		}
		var popup targetInfo
		for _, target := range targets {
			if target.Type == "page" && target.ID != d.rootID && target.WsURL != "" {
				popup = target
				break
			}
		}
		_, activeID, _ := d.session()
		if popup.ID != "" && popup.ID != activeID {
			c, err := dialPage(popup.WsURL)
			if err != nil {
				continue
			}
			if err := d.switchTo(popup.ID, c); err != nil {
				_ = c.conn.Close()
				continue
			}
			wake()
		} else if popup.ID == "" && activeID != d.rootID {
			if err := d.switchTo(d.rootID, d.root); err == nil {
				wake()
			}
		}
	}
}

// ---------- main ----------

func main() {
	port := flag.Int("port", 9222, "chromium remote debugging port")
	url := flag.String("url", "https://en.wikipedia.org", "page to open")
	out := flag.String("out", "", "frame path the viewer watches (required)")
	fifo := flag.String("fifo", "", "FIFO of TAP/SWIPE lines from the viewer")
	selector := flag.String("selector", "", "CSS selector that marks real content")
	waitSec := flag.Int("wait", 30, "max seconds to wait for content")
	refresh := flag.Duration("refresh", 0, "recapture interval (0 = off)")
	oneshot := flag.Bool("oneshot", false, "render once and exit")
	evalExpr := flag.String("eval", "", "run this JS after the wait and print its JSON result")
	quit := flag.Bool("quit", false, "with -oneshot: gracefully Browser.close so state flushes")
	vw := flag.Int("vw", 1620, "viewport width (CSS px)")
	vh := flag.Int("vh", 2160, "viewport height (CSS px)")
	dpr := flag.Float64("dpr", 1, "device pixel ratio")
	adaptersDir := flag.String("adapters", "", "dir of <host>.css paper adapters to inject after settle")
	flag.Parse()
	log.SetFlags(log.Ltime)
	if *out == "" {
		log.Fatal("-out is required")
	}
	if math.IsNaN(*dpr) || math.IsInf(*dpr, 0) || *dpr <= 0 {
		log.Fatal("-dpr must be a positive finite number")
	}

	rootTarget, err := pageTarget(*port, 20*time.Second)
	if err != nil {
		log.Fatalf("browser: %v", err)
	}
	c, err := dialPage(rootTarget.WsURL)
	if err != nil {
		log.Fatalf("dial: %v", err)
	}
	d := &driver{
		c: c, activeID: rootTarget.ID, rootID: rootTarget.ID, root: c,
		dpr: *dpr, cssWidth: *vw, cssHeight: *vh,
		outPath: *out, adapters: loadAdapters(*adaptersDir),
	}

	_ = c.call("Page.enable", nil, nil)
	_ = c.call("Runtime.enable", nil, nil)

	// Publish the top frame's URL next to the frame file on every navigation
	// — the viewer's URL bar pre-fills from it. navigatedWithinDocument
	// matters as much as frameNavigated: SPAs (ChatGPT switching
	// conversations included) move via pushState, not document loads. Only
	// real web URLs are published: a failed nav commits
	// chrome-error://chromewebdata/, which would poison the bar's reload.
	go func() {
		urlPath := filepath.Join(filepath.Dir(*out), "current.url")
		mainFrame := ""
		publish := func(u string) {
			if !strings.HasPrefix(u, "http://") && !strings.HasPrefix(u, "https://") {
				return
			}
			tmp := urlPath + ".tmp"
			if os.WriteFile(tmp, []byte(u+"\n"), 0600) == nil {
				_ = os.Rename(tmp, urlPath)
			}
		}
		for ev := range c.events {
			switch ev.Method {
			case "Page.frameNavigated":
				var p struct {
					Frame struct {
						ID       string `json:"id"`
						URL      string `json:"url"`
						ParentID string `json:"parentId"`
					} `json:"frame"`
				}
				if json.Unmarshal(ev.Params, &p) != nil || p.Frame.ParentID != "" {
					continue
				}
				mainFrame = p.Frame.ID
				publish(p.Frame.URL)
			case "Page.navigatedWithinDocument":
				var p struct {
					FrameID string `json:"frameId"`
					URL     string `json:"url"`
				}
				if json.Unmarshal(ev.Params, &p) != nil || (mainFrame != "" && p.FrameID != mainFrame) {
					continue
				}
				publish(p.URL)
			}
		}
	}()
	// Pin the initial mobile viewport. The viewer later reports the physical
	// page area through VP; setViewport converts it to CSS pixels using DPR.
	if err := d.applyViewport(c); err != nil {
		log.Printf("viewport override: %v", err)
	}

	if _, err := d.navigate(*url); err != nil {
		log.Fatalf("navigate: %v", err)
	}
	d.waitContent(*selector, time.Duration(*waitSec)*time.Second)
	if *evalExpr != "" {
		var v json.RawMessage
		if err := c.evaluate(*evalExpr, true, &v); err != nil {
			log.Printf("eval: %v", err)
		} else {
			fmt.Printf("EVAL %s\n", string(v))
		}
	}
	if _, err := d.capture(); err != nil {
		log.Fatalf("capture: %v", err)
	}
	fmt.Println("RENDERED")
	if *oneshot {
		if *quit {
			// Graceful shutdown flushes cookie/session databases to disk —
			// SIGTERM on headless chromium does not.
			_ = c.call("Browser.close", nil, nil)
			select {
			case <-c.dead:
			case <-time.After(10 * time.Second):
			}
		}
		return
	}

	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, syscall.SIGTERM, syscall.SIGINT)

	// FIFO reader. O_RDWR so the fd stays open across writer churn (each
	// viewer write opens/closes the path) instead of streaming EOFs.
	lines := make(chan string, 32)
	if *fifo != "" {
		go func() {
			var f *os.File
			var err error
			for i := 0; i < 50; i++ {
				f, err = os.OpenFile(*fifo, os.O_RDWR, 0)
				if err == nil {
					break
				}
				time.Sleep(200 * time.Millisecond)
			}
			if err != nil {
				log.Printf("fifo: %v", err)
				return
			}
			buf := make([]byte, 256)
			partial := ""
			for {
				n, err := f.Read(buf)
				if err != nil {
					log.Printf("fifo read: %v", err)
					return
				}
				partial += string(buf[:n])
				for {
					i := strings.IndexByte(partial, '\n')
					if i < 0 {
						break
					}
					line := strings.TrimSpace(partial[:i])
					partial = partial[i+1:]
					if line != "" {
						lines <- line
					}
				}
			}
		}()
	}

	var tick <-chan time.Time
	if *refresh > 0 {
		t := time.NewTicker(*refresh)
		defer t.Stop()
		tick = t.C
	}

	// Every screenshot goes through one worker goroutine so the command loop
	// never blocks behind a capture: gestures reach the page the moment they
	// arrive instead of queueing seconds behind sleeps and encodes. A gesture
	// schedules a capture burst at +180ms/+880ms (the page usually reacts
	// within a moment). Gestures landing mid-burst never delay its timers —
	// the glass keeps painting during a tap flurry — they just extend the
	// burst by one coalesced pass so the last gesture also gets a settled
	// frame. The -refresh ticker catches reactions slower than the burst;
	// without a ticker the burst keeps the old +4s straggler pass.
	activity := make(chan struct{}, 1) // gesture replayed: run/extend a burst
	snapOnce := make(chan struct{}, 1) // capture once, soon (ticker, post-nav)
	poke := func(ch chan struct{}) {
		select {
		case ch <- struct{}{}:
		default:
		}
	}
	go monitorPopups(*port, d, func() { poke(activity) })
	go func() {
		burst := []time.Duration{180 * time.Millisecond, 700 * time.Millisecond}
		if *refresh <= 0 {
			burst = append(burst, 2500*time.Millisecond)
		}
		snap := func() {
			if d.navigating.Load() {
				return
			}
			if _, err := d.capture(); err != nil {
				log.Printf("capture: %v", err)
			}
		}
		for {
			select {
			case <-activity:
				for again := true; again; {
					again = false
					for _, delay := range burst {
						t := time.NewTimer(delay)
						for waiting := true; waiting; {
							select {
							case <-activity:
								again = true // extend; this timer keeps its schedule
							case <-t.C:
								waiting = false
							}
						}
						snap()
					}
				}
			case <-snapOnce:
				snap()
			}
		}
	}()

	// Navigation worker: NAV must not freeze the command loop — a slow page
	// would otherwise queue every tap and keystroke for up to -wait seconds
	// and replay them against the wrong document. One nav runs at a time; a
	// NAV arriving mid-nav replaces any still-queued one (latest wins).
	navReq := make(chan string, 1)
	go func() {
		for u := range navReq {
			d.navGen.Add(1)
			d.navigating.Store(true)
			sameDoc, err := d.navigate(u)
			if err != nil {
				log.Printf("navigate: %v", err)
			} else if !sameDoc {
				// The -selector is tuned for the launch URL; an arbitrary
				// destination may never match it and would stall until
				// -wait. body always exists, so this waits for load + paint
				// settle (capped: manual navs answer to an impatient human)
				// and the -refresh ticker catches late-hydrating SPAs.
				w := time.Duration(*waitSec) * time.Second
				if w > 30*time.Second {
					w = 30 * time.Second
				}
				d.waitContent("main, body", w) // waitContent applies the adapter
			} else {
				d.applyAdapter() // same-doc route change: reskin the new view
			}
			d.navigating.Store(false)
			// Always publish something — a failed nav commits Chromium's
			// error page, and stale glass with no feedback is worse.
			poke(snapOnce)
		}
	}()

	for {
		select {
		case <-sigs:
			log.Print("terminating")
			return
		case <-c.dead:
			log.Printf("browser connection lost: %v", c.deadErr)
			os.Exit(1) // the takeover script shows the error screen on rc!=0
		case line := <-lines:
			var x1, y1, x2, y2 int
			switch {
			case strings.HasPrefix(line, "TAP "):
				if _, err := fmt.Sscanf(line, "TAP %d %d", &x1, &y1); err == nil {
					log.Printf("tap %d,%d", x1, y1)
					d.tap(x1, y1)
					poke(activity)
				}
			case strings.HasPrefix(line, "SWIPE "):
				if _, err := fmt.Sscanf(line, "SWIPE %d %d %d %d", &x1, &y1, &x2, &y2); err == nil {
					log.Printf("swipe %d,%d -> %d,%d", x1, y1, x2, y2)
					d.swipe(x1, y1, x2, y2)
					poke(activity)
				}
			case strings.HasPrefix(line, "NAV "):
				u := strings.TrimSpace(strings.TrimPrefix(line, "NAV "))
				// Host only: URLs can embed credentials and share tokens,
				// and this log is world-readable next to the frame file.
				if pu, err := neturl.Parse(u); err == nil && pu.Host != "" {
					log.Printf("nav %s", pu.Host)
				} else {
					log.Printf("nav (%d chars)", len(u))
				}
				select {
				case navReq <- u:
				default: // a nav is queued: replace it with the newest
					select {
					case <-navReq:
					default:
					}
					select {
					case navReq <- u:
					default:
					}
				}
			case line == "BACK":
				active, _, _ := d.session()
				_ = active.evaluateT("history.back()", false, nil, 5*time.Second)
				time.Sleep(400 * time.Millisecond) // let the prior page restore
				d.applyAdapter()
				poke(activity)
			case strings.HasPrefix(line, "KEY "):
				var cp, mods int
				if _, err := fmt.Sscanf(line, "KEY %d %d", &cp, &mods); err == nil && cp > 0 {
					log.Printf("key mods=%d", mods) // no codepoint: could be a password
					d.sendKey(charKey(rune(cp)), mods)
					poke(activity)
				}
			case strings.HasPrefix(line, "KEYN "):
				var name string
				var mods int
				if _, err := fmt.Sscanf(line, "KEYN %s %d", &name, &mods); err == nil {
					if k, ok := namedKeys[name]; ok {
						log.Printf("keyn %s mods=%d", name, mods)
						d.sendKey(k, mods)
						poke(activity)
					}
				}
			case strings.HasPrefix(line, "VP "):
				var w, h int
				if _, err := fmt.Sscanf(line, "VP %d %d", &w, &h); err == nil && w > 0 && h > 0 {
					log.Printf("viewport %dx%d", w, h)
					if err := d.setViewport(w, h); err != nil {
						log.Printf("viewport override: %v", err)
					}
					poke(activity)
				}
			}
		case <-tick:
			poke(snapOnce)
		}
	}
}
