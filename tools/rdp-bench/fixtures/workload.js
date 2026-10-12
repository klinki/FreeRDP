/* Browser workload prototype: deterministic logical content, not a finished RDP test runner. */
'use strict';
(() => {
  const M = window.RdpBenchModel;
  const el = id => document.getElementById(id);
  const motion = el('motion'), ctx = motion.getContext('2d');
  const marker = el('marker'), mark = marker.getContext('2d');
  const video = el('movie');
  const scenes = new Set(['text-scroll','web-scroll','canvas-motion','typing-visual','static-idle','video']);
  const MAX_EVENTS=100000;
  let cfg=null, active=false, origin=0, lastTick=-1, raf=0, videoCallback=null, probeId=0, keyBuffer=null;
  let events=[], droppedEvents=0, currentScene='static-idle';
  let mediaTick=0, mediaTime=0, callbacks=0, sourceSkippedTicks=0;
  function log(type, data={}) {
    const entry={type, source:'windows_fixture', sourcePerformanceMs:performance.now(),
      runTag:cfg?.runTag ?? null, phaseId:cfg?.phaseId ?? null, scene:currentScene, ...data};
    if (events.length < MAX_EVENTS) events.push(entry); else droppedEvents++;
  }
  function fit() {
    const dpr=window.devicePixelRatio || 1;
    motion.width=Math.round(innerWidth*dpr); motion.height=Math.round(innerHeight*dpr);
    ctx.setTransform(dpr,0,0,dpr,0,0);
    log('viewport',{width:innerWidth,height:innerHeight,dpr});
  }
  function show(name) {
    el('document').classList.toggle('hidden',!['text-scroll','web-scroll','static-idle'].includes(name));
    el('lines').classList.toggle('hidden',name!=='text-scroll');
    el('article').classList.toggle('hidden',!['web-scroll','static-idle'].includes(name));
    motion.classList.toggle('hidden',name!=='canvas-motion');
    video.classList.toggle('hidden',name!=='video');
    el('typing').classList.toggle('hidden',name!=='typing-visual');
    marker.classList.toggle('hidden',name==='static-idle' || cfg?.markers===false);
  }
  function drawMarker(tick) {
    if (!cfg || cfg.markers===false || currentScene==='static-idle') return;
    const b=M.markerBytes(cfg.runTag,cfg.phaseId,tick>>>0,probeId>>>0);
    mark.fillStyle='#fff';mark.fillRect(0,0,112,64);
    mark.fillStyle='#000';mark.fillRect(4,4,104,56);
    for (let i=0;i<128;i++) {
      mark.fillStyle=((b[i>>3] >> (7-(i&7)))&1)?'#fff':'#000';
      mark.fillRect(8+(i%16)*6,8+Math.floor(i/16)*6,6,6);
    }
  }
  const text=Array.from({length:1200},(_,i)=>`${String(i).padStart(4,'0')}  public async Task<Result> Benchmark_${i}()   // Repeatable text: příliš žluťoučký kůň`).join('\n');
  el('lines').textContent=text;
  el('article').innerHTML='<h1>Local benchmark article and dashboard</h1><p>This page uses fixed local content. No advertising, network APIs, or remote fonts.</p>'+
    Array.from({length:30},(_,i)=>`<section><h2>Section ${i+1}: a fixed local workflow</h2><p>Compare the same source content, viewport, browser and network conditions. A faster stream is valuable only if interaction stays responsive and the image remains useful.</p><div class="cards">`+
      [1,2,3].map(j=>`<div class="card"><h3>Metric ${i*3+j}</h3><p>Fixed text, layout, and image-like blocks for deterministic scrolling.</p><div style="height:60px;background:linear-gradient(${30+i*3}deg,#6b89a7,#dbe4ed)"></div></div>`).join('')+'</div></section>').join('');
  function render(tick) {
    const seconds=tick/cfg.targetFps;
    if (currentScene==='text-scroll' || currentScene==='web-scroll') {
      const doc=el('document');
      const span=Math.max(1,doc.scrollHeight-doc.clientHeight);
      // Triangular scroll stays continuous at the turnaround; never catches up by replaying actions.
      const pos=(seconds*cfg.scrollPixelsPerSecond)%(2*span);
      doc.scrollTop=pos<=span?pos:2*span-pos;
    } else if (currentScene==='canvas-motion') {
      ctx.fillStyle='#172334';ctx.fillRect(0,0,innerWidth,innerHeight);
      for(let i=0;i<24;i++) {
        const offset=M.randomAt(cfg.seed,0,i)*Math.PI*2;
        const x=(0.5+0.42*Math.sin(seconds*(0.5+i*.013)+offset))*innerWidth;
        const y=(0.5+0.42*Math.cos(seconds*(0.35+i*.019)+offset))*innerHeight;
        ctx.fillStyle=`hsl(${(i*31+seconds*30)%360},70%,60%)`;
        ctx.fillRect(x-60,y-60,120,120);
      }
    } else if (currentScene==='typing-visual') {
      const base='This is a repeatable editing workload.\nIt generates visual work on Windows; use separate Mac input probes for end-to-end input latency.\n';
      const content=base.repeat(20), count=Math.floor(seconds*12)%(content.length+1);
      el('typing').textContent=content.slice(0,count);
    }
    drawMarker(tick);
  }
  function frame(now) {
    if(!active) return;
    callbacks++;
    const tick=M.tickAt(Math.max(0,now-origin),cfg.targetFps);
    if(tick!==lastTick) {
      if(lastTick>=0 && tick>lastTick+1) sourceSkippedTicks+=tick-lastTick-1;
      lastTick=tick;
      if(currentScene!=='video' && currentScene!=='static-idle') {
        render(tick);
        log('frame_intent',{tick,sourceSkippedTicks,callbacks});
      }
    }
    raf=requestAnimationFrame(frame);
  }
  function observeMedia(now,meta) {
    if(!active || currentScene!=='video') return;
    mediaTime=meta.mediaTime;
    mediaTick=meta.presentedFrames>>>0; // Monotonic submission identity; mediaTime may loop.
    drawMarker(mediaTick);
    log('video_callback',{mediaTime,mediaTick,mediaFrameIndex:Math.floor(mediaTime*cfg.videoFps+0.0001),presentedFrames:meta.presentedFrames,
      expectedDisplayTime:meta.expectedDisplayTime,presentationTime:meta.presentationTime});
    videoCallback=video.requestVideoFrameCallback(observeMedia);
  }
  async function prepareVideo(url) {
    if(!url) throw new Error('A pinned local video URL is required. No substitute animation is used.');
    if(typeof video.requestVideoFrameCallback!=='function') throw new Error('requestVideoFrameCallback is unavailable');
    const u=new URL(url,location.href);
    if(u.origin!==location.origin) throw new Error('Prototype requires same-origin local video');
    video.src=u.href;
    await new Promise((resolve,reject)=>{
      const timer=setTimeout(()=>done(new Error('Video readiness timeout')),15000);
      const ok=()=>done(); const bad=()=>done(new Error('Video unavailable or codec unsupported'));
      function done(error) {clearTimeout(timer);video.removeEventListener('loadeddata',ok);video.removeEventListener('error',bad);error?reject(error):resolve();}
      video.addEventListener('loadeddata',ok);video.addEventListener('error',bad);video.load();
    });
  }
  function stop() {
    if(active) log('phase_stop',{lastTick,mediaTick,mediaTime,callbacks,sourceSkippedTicks});
    active=false;cancelAnimationFrame(raf);
    if(videoCallback!==null && video.cancelVideoFrameCallback) video.cancelVideoFrameCallback(videoCallback);
    videoCallback=null;video.pause();
    el('controls').classList.remove('hidden');
  }
  async function start(options={}) {
    stop();
    const next={scene:'text-scroll',phaseId:1,runTag:1,seed:42,targetFps:60,
      scrollPixelsPerSecond:400,videoFps:30,markers:true,...options};
    if(!scenes.has(next.scene)) throw new Error('Unknown scene');
    for(const key of ['targetFps','scrollPixelsPerSecond','videoFps'])
      if(!Number.isFinite(next[key]) || next[key]<=0) throw new Error(`Invalid ${key}`);
    if(next.targetFps>240) throw new Error('targetFps must be <=240');
    M.markerBytes(next.runTag,next.phaseId,0,0);
    cfg=next;currentScene=next.scene;probeId=0;keyBuffer=null;
    await document.fonts.ready;
    if(currentScene==='video') await prepareVideo(cfg.videoUrl);
    show(currentScene);fit();el('document').scrollTop=0;
    el('controls').classList.add('hidden');
    origin=performance.now();lastTick=-1;callbacks=0;sourceSkippedTicks=0;mediaTick=0;mediaTime=0;
    active=true;log('phase_begin',{configuration:cfg,viewport:{width:innerWidth,height:innerHeight,dpr:devicePixelRatio},visible:document.visibilityState});
    try {
      if(currentScene==='video') {video.currentTime=0;video.loop=true;videoCallback=video.requestVideoFrameCallback(observeMedia);await video.play();}
      else render(0);
      if(currentScene!=='static-idle') raf=requestAnimationFrame(frame);
      return {sourcePerformanceMs:origin,phaseId:cfg.phaseId,runTag:cfg.runTag};
    } catch(error) {log('phase_error',{error:String(error)});stop();throw error;}
  }
  addEventListener('keydown',e=>{
    if(e.key==='Escape') {stop();return;}
    if(!active) return;
    if(e.key==='!') {keyBuffer='';return;}
    if(keyBuffer===null) return;
    if(e.key==='Enter') {
      if(/^[0-9a-fA-F]{8}$/.test(keyBuffer)) {
        probeId=parseInt(keyBuffer,16)>>>0;
        log('probe_received',{probeId,token:keyBuffer});
        drawMarker(currentScene==='video'?mediaTick:Math.max(0,lastTick));
        log('probe_marker_write',{probeId});
      } else log('invalid_probe',{length:keyBuffer.length});
      keyBuffer=null;return;
    }
    if(e.key.length===1) {
      if(/^[0-9a-fA-F]$/.test(e.key) && keyBuffer.length<8) keyBuffer+=e.key; else keyBuffer=null;
    }
  });
  for(const kind of ['waiting','playing','pause','ended','stalled','error'])
    video.addEventListener(kind,()=>log('video_'+kind,{mediaTime:video.currentTime}));
  addEventListener('resize',()=>{fit();if(active && currentScene!=='video')render(Math.max(0,lastTick));});
  document.addEventListener('visibilitychange',()=>log('visibility',{state:document.visibilityState}));
  window.rdpBench={
    ready:async()=>{await document.fonts.ready;return {version:1,scenes:[...scenes],viewport:{width:innerWidth,height:innerHeight,dpr:devicePixelRatio}};},
    start,stop,
    mark:(phaseId)=>{if(!cfg || !Number.isInteger(phaseId) || phaseId<0 || phaseId>65535) throw new Error("Invalid phase mark"); cfg.phaseId=phaseId;drawMarker(currentScene==='video'?mediaTick:Math.max(0,lastTick));log("generation_mark",{phaseId});return {phaseId};},
    status:()=>({active,configuration:cfg,lastTick,mediaTick,mediaTime,callbacks,sourceSkippedTicks,queuedEvents:events.length,droppedEvents}),
    drainEvents:()=>{const out={events,droppedEvents};events=[];droppedEvents=0;return out;}
  };
  el('start').addEventListener('click',async()=>{
    el('error').textContent='';
    try {await start({scene:el('scene').value,videoUrl:el('video-url').value});}
    catch(error) {el('error').textContent=String(error);}
  });
  show('static-idle');fit();
})();
