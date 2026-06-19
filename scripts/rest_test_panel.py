#!/usr/bin/env python3
"""Temporary control panel for the fm-sdr-tuner anonymous REST API.

Serves a small static web page that talks to the tuner's REST control API
(via the browser's fetch(), which works cross-origin because the API sends
permissive CORS headers). Use it to exercise the SDR settings the way an
fm-dx-webserver plugin eventually will, against either the RTL-SDR or the
SDRplay backend.

Enable the API in fm-sdr-tuner.ini:

    [rest]
    enabled = true
    port = 8080
    bind_address = 127.0.0.1

Then run:

    python3 scripts/rest_test_panel.py --api http://127.0.0.1:8080 --port 8090

and open http://127.0.0.1:8090 in a browser. The page also works as a pure
client: every control issues GET/POST to <api>/api/control and polls
<api>/api/status once a second.
"""
import argparse
import http.server
import socketserver

PAGE = """<!doctype html>
<html><head><meta charset="utf-8"><title>fm-sdr-tuner control</title>
<style>
 body{font-family:system-ui,sans-serif;max-width:640px;margin:2rem auto;padding:0 1rem}
 fieldset{margin:1rem 0;border:1px solid #ccc;border-radius:8px}
 label{display:inline-block;min-width:9rem}
 input[type=number]{width:8rem}
 button{margin:.2rem;padding:.3rem .8rem;border:1px solid #aaa;border-radius:5px;background:#f4f4f4;cursor:pointer}
 button.active{background:#2e7d32;color:#fff;border-color:#1b5e20;font-weight:bold}
 button.big{font-size:1rem;padding:.5rem 1.1rem;background:#b71c1c;color:#fff;border:none;border-radius:6px;margin:.3rem .3rem 1rem 0}
 button.blue{background:#0277bd}
 pre{background:#111;color:#0f0;padding:1rem;border-radius:8px;overflow:auto}
 .row{margin:.4rem 0}
 .stats{position:sticky;top:0;z-index:10;display:grid;grid-template-columns:repeat(5,1fr);
        gap:.3rem .6rem;background:#0d1b2a;color:#e8eef5;border-radius:10px;
        padding:.7rem .9rem;margin:.3rem 0 .6rem}
 .stats .cell{display:flex;flex-direction:column}
 .stats .k{font-size:.62rem;letter-spacing:.04em;color:#8aa0b8;text-transform:uppercase}
 .stats .v{font-size:1.15rem;font-weight:700;line-height:1.2}
 .stats .cell.warn{background:#b71c1c;border-radius:6px;padding:0 .3rem;margin:-.1rem -.3rem}
 .stats .cell.good .v{color:#5ce08a}
</style></head><body>
<h1>fm-sdr-tuner control</h1>
<p>API: <code id="api"></code> &mdash; fields apply on <b>Enter</b>; buttons on click. Live state is highlighted; controls match the connected device.</p>
<p id="device" style="font-weight:bold;margin:.2rem 0"></p>
<div class="stats">
 <div class="cell"><span class="k">Signal</span><span class="v" id="vSignal">–</span></div>
 <div class="cell"><span class="k">dBFS</span><span class="v" id="vDbfs">–</span></div>
 <div class="cell"><span class="k">SNR</span><span class="v" id="vSnr">–</span></div>
 <div class="cell"><span class="k">Peak dev</span><span class="v" id="vPeak">–</span></div>
 <div class="cell" id="cOverload"><span class="k">Overload</span><span class="v" id="vOverload">–</span></div>
 <div class="cell"><span class="k">Stereo</span><span class="v" id="vStereo">–</span></div>
 <div class="cell"><span class="k">Pilot</span><span class="v" id="vPilot">–</span></div>
 <div class="cell"><span class="k">RDS dev</span><span class="v" id="vRdsDev">–</span></div>
 <div class="cell"><span class="k">RDS BER</span><span class="v" id="vBer">–</span></div>
 <div class="cell"><span class="k">RDS grps</span><span class="v" id="vGroups">–</span></div>
</div>
<button class="big" onclick="resetDefaults()">Reset to sane defaults</button>
<button class="big blue" onclick="resetStats()">Reset stats (MPX/RDS window)</button>
<fieldset><legend>Tuning</legend>
 <div class="row"><label>Frequency (kHz)</label>
   <input type="number" id="freq" step="100" onchange="send({freq_khz:+this.value})"></div>
 <div class="row"><label>Bandwidth (kHz)</label>
   <input type="number" id="bw" step="10" onchange="send({bandwidth_khz:+this.value})">
   <small>0 = auto/widest</small></div>
</fieldset>
<fieldset><legend>Gain</legend>
 <div class="row"><label>Auto gain (AGC)</label>
   <button id="agcOn" onclick="send({agc:true})">On</button>
   <button id="agcOff" onclick="send({agc:false})">Off</button></div>
 <div class="row"><label>Manual gain (dB)</label>
   <input type="number" id="gain" step="1" onchange="send({gain_db:+this.value})">
   <small>used when AGC is Off</small></div>
</fieldset>
<fieldset id="sdrplaySection"><legend>SDRplay (RSP)</legend>
 <div class="row"><label>LNA state</label>
   <input type="number" id="lna" min="0" max="27" onchange="send({lna:+this.value})">
   <small>0 = most gain</small></div>
 <div class="row" id="antRow"><label>Antenna</label>
   <input type="number" id="ant" min="0" max="2" onchange="send({antenna:+this.value})"></div>
 <div class="row"><label>Bias-T</label>
   <button id="biasOn" onclick="send({bias_tee:true})">On</button>
   <button id="biasOff" onclick="send({bias_tee:false})">Off</button></div>
</fieldset>
<fieldset id="rtlSection"><legend>RTL-SDR</legend>
 <div class="row"><label>Freq correction (ppm)</label>
   <input type="number" id="ppm" min="-250" max="250" onchange="send({ppm:+this.value})"></div>
 <div class="row"><label>RTL digital AGC</label>
   <button id="rtlAgcOn" onclick="send({rtl_agc:true})">On</button>
   <button id="rtlAgcOff" onclick="send({rtl_agc:false})">Off</button></div>
</fieldset>
<fieldset><legend>Audio</legend>
 <div class="row"><label>De-emphasis</label>
   <button id="deem0" onclick="send({deemphasis:0})">50us</button>
   <button id="deem1" onclick="send({deemphasis:1})">75us</button>
   <button id="deem2" onclick="send({deemphasis:2})">Off</button></div>
 <div class="row"><label>Stereo blend</label>
   <input type="range" id="blend" min="0" max="2" step="1" style="width:8rem;vertical-align:middle"
          oninput="document.getElementById('blendLbl').textContent=['soft','normal','aggressive'][this.value]"
          onchange="send({blend:+this.value})">
   <span id="blendLbl" style="margin-left:.5rem">normal</span></div>
 <div class="row"><label>Force mono</label>
   <button id="monoOn" onclick="send({force_mono:true})">On</button>
   <button id="monoOff" onclick="send({force_mono:false})">Off</button></div>
 <div class="row"><label>Volume</label>
   <input type="number" id="vol" min="0" max="100" onchange="send({volume:+this.value})"></div>
 <div class="row"><label>Tuner</label>
   <button id="runOn" onclick="send({action:'start'})">Start</button>
   <button id="runOff" onclick="send({action:'stop'})">Stop</button></div>
</fieldset>
<h3>Status (raw)</h3>
<pre id="out">(loading...)</pre>
<script>
 const API = location.search.replace(/^\\?api=/, '') || "__API__";
 const $ = id => document.getElementById(id);
 $('api').textContent = API;
 // Pause status polling briefly after a manual edit so it doesn't overwrite the
 // field/response the user just acted on.
 let lastAction = 0;
 const DEFAULTS = {freq_khz:98000, bandwidth_khz:0, agc:true, lna:4, antenna:0,
                   bias_tee:false, ppm:0, rtl_agc:false, deemphasis:0,
                   force_mono:false, volume:80};
 function setField(id,val){ const e=$(id); if(e && document.activeElement!==e && val!==undefined) e.value=val; }
 function setActive(id,on){ const e=$(id); if(e) e.classList.toggle('active',!!on); }
 function refresh(s){
   if(!s) return;
   const sdrplay = (s.source === 'sdrplay');
   // Show only the controls that apply to the connected device.
   $('sdrplaySection').style.display = sdrplay ? '' : 'none';
   $('rtlSection').style.display = sdrplay ? 'none' : '';
   $('antRow').style.display = (s.antenna_count > 1) ? '' : 'none';
   $('device').textContent = (sdrplay ? 'SDRplay ' : 'RTL-SDR ') +
     (s.model||s.source||'') + ' — ' + (s.running ? 'streaming' : 'stopped') +
     ' @ ' + ((s.frequency_hz/1e6).toFixed(2)) + ' MHz';
   const setV = (id,v) => { const e=$(id); if(e) e.textContent=v; };
   setV('vSignal', (s.signal||0).toFixed(0)+'/120');
   setV('vDbfs', s.dbfs!==undefined ? s.dbfs.toFixed(1) : '–');
   setV('vSnr', s.snr!==undefined ? s.snr.toFixed(1)+' dB' : '–');
   setV('vPeak', s.mpx_peak_khz!==undefined ? '±'+s.mpx_peak_khz.toFixed(1)+'k' : '–');
   setV('vOverload', s.overload ? 'YES' : 'no');
   const cOv=$('cOverload'); if(cOv) cOv.classList.toggle('warn', !!s.overload);
   setV('vStereo', s.stereo ? 'STEREO' : 'mono');
   const cSt=document.querySelector('#vStereo'); if(cSt) cSt.parentElement.classList.toggle('good', !!s.stereo);
   setV('vPilot', (s.pilot_khz!==undefined ? '±'+s.pilot_khz.toFixed(1)+'k' : '–'));
   setV('vRdsDev', s.rds_dev_khz!==undefined ? '±'+s.rds_dev_khz.toFixed(1)+'k' : '–');
   setV('vBer', s.rds_ber!==undefined ? (100*s.rds_ber).toFixed(2)+'%' : '–');
   setV('vGroups', s.rds_groups!==undefined ? s.rds_groups : '–');
   setField('freq', Math.round(s.frequency_hz/1000));
   setField('bw', Math.round(s.bandwidth_hz/1000));
   setField('gain', s.gain_db);
   setField('lna', s.lna);
   setField('ant', s.antenna);
   setField('ppm', s.ppm);
   setField('vol', s.volume);
   setActive('agcOn', s.auto_gain); setActive('agcOff', !s.auto_gain);
   setActive('biasOn', s.bias_tee); setActive('biasOff', !s.bias_tee);
   setActive('rtlAgcOn', s.rtl_agc); setActive('rtlAgcOff', !s.rtl_agc);
   setActive('monoOn', s.force_mono); setActive('monoOff', !s.force_mono);
   if(s.blend!==undefined){ setField('blend', s.blend);
     const bl=$('blendLbl'); if(bl) bl.textContent=['soft','normal','aggressive'][s.blend]||s.blend; }
   setActive('deem0', s.deemphasis===0); setActive('deem1', s.deemphasis===1);
   setActive('deem2', s.deemphasis===2);
   setActive('runOn', s.running); setActive('runOff', !s.running);
 }
 async function send(obj){
   lastAction = Date.now();
   try{
     const r = await fetch(API+"/api/control",{method:"POST",
       headers:{"Content-Type":"application/json"},body:JSON.stringify(obj)});
     const j = await r.json();
     $('out').textContent = JSON.stringify(j,null,2);
     if(j.status) refresh(j.status);
   }catch(e){ $('out').textContent = "error: "+e; }
 }
 function resetDefaults(){ send(DEFAULTS); }
 function resetStats(){ send({action:'reset_stats'}); }
 async function poll(){
   if(Date.now()-lastAction < 1500) return;  // keep the last action's response visible
   try{
     const r = await fetch(API+"/api/status");
     const s = await r.json();
     $('out').textContent = JSON.stringify(s,null,2);
     refresh(s);
   }catch(e){}
 }
 setInterval(poll, 1000); poll();
</script>
</body></html>
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--api", default="http://127.0.0.1:8080",
                    help="Base URL of the fm-sdr-tuner REST API")
    ap.add_argument("--port", type=int, default=8090,
                    help="Port to serve this control panel on")
    args = ap.parse_args()

    html = PAGE.replace("__API__", args.api).encode("utf-8")

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(html)))
            self.end_headers()
            self.wfile.write(html)

        def log_message(self, format, *args):  # noqa: A002
            pass

    # Allow immediate rebind (avoids "Address already in use" from a prior
    # instance's TIME_WAIT socket on a quick restart).
    socketserver.TCPServer.allow_reuse_address = True
    try:
        httpd = socketserver.TCPServer(("127.0.0.1", args.port), Handler)
    except OSError as e:
        print(f"Could not bind 127.0.0.1:{args.port} ({e}).")
        print(f"Another panel is probably already running on that port — stop it "
              f"(pkill -f rest_test_panel) or pass --port <other>.")
        raise SystemExit(1)
    with httpd:
        print(f"Control panel: http://127.0.0.1:{args.port}  ->  API {args.api}")
        print("Ctrl+C to stop.")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
