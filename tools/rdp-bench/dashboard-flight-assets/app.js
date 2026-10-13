"use strict";
const LOSSES=[0,25,50,70],WINDOWS=[16,32,48,64],COLORS={16:"#14856c",32:"#417cc8",48:"#8866bd",64:"#cf8840"};
const DATASETS={local:{label:"Local recovery branch",json:"data.json",csv:"trials.csv"},pr13608:{label:"PR #13608",json:"data-pr13608.json",csv:"trials-pr13608.csv"}};
const METRICS={goodput_mbps:["Verified application payload","Mbit/s"],rtt_p95_ms:["Observed probe RTT p95","ms"],rtt_p99_ms:["Observed probe RTT p99","ms"],scheduled_p99_ms:["Scheduled response p99","ms"]};
const el=id=>document.getElementById(id),finite=v=>typeof v==="number"&&Number.isFinite(v),fmt=(v,d=2)=>finite(v)?v.toLocaleString("en-US",{maximumFractionDigits:d,minimumFractionDigits:d}):"—",count=v=>finite(v)?v.toLocaleString("en-US"):"—";
let DATA={meta:{status:"preparing",planned_trials:80},groups:[],trials:[],network_checks:{status:"pending",samples:[]}},state={loss:0,window:16,metric:"goodput_mbps",cohort:"diagnostic",implementation:"pr13608",visible:new Set(WINDOWS)};
let datasetRequest=0;
function group(loss,window){return DATA.groups.find(g=>g.loss_percent===loss&&g.window_packets===window)||{loss_percent:loss,window_packets:window,planned_count:5,run_count:0,metrics:{}}}
function metric(g,key){return g.metrics?.[state.cohort]?.[key]||{median:null,n:0}}
function setText(id,text){el(id).textContent=text}
function element(tag,text,cls){const node=document.createElement(tag);if(text!==undefined)node.textContent=text;if(cls)node.className=cls;return node}
function addStat(id,value,unit=""){const n=el(id);n.replaceChildren(document.createTextNode(value));if(unit)n.append(element("em"," "+unit))}
function trialIssue(t){return t.outcome!=="NOT_RUN"&&(t.outcome!=="COMPLETE"||t.qualified!==true||t.validity==="INVALID_ARTIFACT"||t.validity==="INVALID_HARNESS")}
function operationalFailure(t){const direct=t.operational_failure;if(direct&&typeof direct==="object")return direct;if(typeof direct==="string")return {stage:"reported",reason:direct};const records=DATA.meta?.operational_failures;if(Array.isArray(records))return records.find(r=>r&&r.trial_id===t.trial_id)||null;if(records&&typeof records==="object")return records[t.trial_id]||null;return null}
function startupState(t){const failure=operationalFailure(t);if(failure)return failure.stage==="startup";return Array.isArray(t.endpoint_receipts)&&t.endpoint_receipts.length?false:null}
function boolAccounting(rows,key){const known=rows.filter(t=>typeof t[key]==="boolean");return {count:known.length?known.filter(t=>t[key]).length:null,known:known.length,unknown:rows.length-known.length}}
function startupAccounting(rows){const states=rows.map(startupState),known=states.filter(v=>typeof v==="boolean");return {count:known.length?known.filter(Boolean).length:null,unknown:states.length-known.length}}
function probeAccounting(rows){const keys=["expected","injected","observed","missed","unobserved","timeouts"],known=rows.filter(t=>keys.every(k=>Number.isInteger(t.probes?.[k])&&t.probes[k]>=0));return {known:known.length,total:rows.length,values:Object.fromEntries(keys.map(k=>[k,known.length?known.reduce((sum,t)=>sum+t.probes[k],0):null]))}}
function trialStatus(t){if(t.outcome==="NOT_RUN")return "NOT_RUN";const notes=[],operational=operationalFailure(t);if(operational?.stage==="startup")notes.push("startup initialization failure");if(t.validity==="INVALID_ARTIFACT")notes.push("artifact invalid");else if(t.validity==="INVALID_HARNESS")notes.push(t.driver_failure?"benchmark driver failure":"timing/harness invalid");else if(t.qualified!==true)notes.push("unqualified");if(t.deadline===true)notes.push("benchmark deadline cancellation");if(t.native_failure===true)notes.push("native transport failed");if(t.outcome!=="COMPLETE"&&t.outcome!=="FAIL"&&t.outcome!=="HARNESS_FAILURE")notes.push("incomplete outcome");if(t.native_failure===null)notes.push("native health unknown");if(t.deadline===null)notes.push("deadline unknown");return [t.outcome||"UNKNOWN",...notes].join(" · ")}
function trialReasons(t){const reasons=[...(t.reasons||[]),...(t.artifact_errors||[])],operational=operationalFailure(t);if(operational){if(typeof operational.reason==="string")reasons.unshift(`${operational.stage||"Operational failure"}: ${operational.reason}`);if(typeof operational.source==="string")reasons.push(`Operational evidence: ${operational.source}${typeof (operational.sha256||operational.source_sha256)==="string"?" · SHA256 "+(operational.sha256||operational.source_sha256):""}`)}if(t.deadline===true)reasons.push("Benchmark deadline bounded an incomplete drain; native transport failure is counted independently");if(t.native_failure===null)reasons.push("Native failure state unavailable");if(t.deadline===null)reasons.push("Deadline state unavailable");if(t.outcome!=="NOT_RUN"&&t.outcome!=="COMPLETE"&&t.outcome!=="FAIL"&&t.outcome!=="HARNESS_FAILURE")reasons.push("Trial outcome is incomplete or unknown");return [...new Set(reasons)].join("; ")}

function payloadCeilings(rows){return rows.filter(t=>!(t.artifact_errors||[]).length&&finite(t.wire_max_payload)&&t.wire_max_payload>0).map(t=>t.wire_max_payload)}
function rangeText(values){const known=[...new Set(values.filter(finite))].sort((a,b)=>a-b);return known.length===1?count(known[0]):known.length?`${count(known[0])}–${count(known[known.length-1])}`:"—"}
function geometryText(name){const values=DATA.trials.map(t=>t.flight_config?.fields?.[name]).filter(finite);if(values.length)return rangeText(values);const g=DATA.meta?.native_geometry||DATA.plan?.native_geometry;return count(g?.[name])}
function driverAccounting(rows){const known=rows.filter(t=>t.driver_failure===true||(typeof t.driver_failure==="boolean"&&Array.isArray(t.endpoint_receipts)&&t.endpoint_receipts.length&&t.endpoint_receipts.every(e=>e.native_driver_error_available===true)));return {count:known.length?known.filter(t=>t.driver_failure===true).length:null,known:known.length,unknown:rows.length-known.length}}
function renderHeader(){
 const trials=DATA.trials,m=DATA.meta||{},dataset=DATASETS[state.implementation],unavailable=m.status==="unavailable",loading=m.status==="loading";
 const native=boolAccounting(trials,"native_failure"),timing=boolAccounting(trials,"timing_valid"),deadline=boolAccounting(trials,"deadline"),startup=startupAccounting(trials),driver=driverAccounting(trials);
 setText("qualification-summary",trials.length?`${trials.filter(t=>t.integrity_verified===true).length}/${trials.length} full streams verified · ${trials.filter(t=>t.integrity_valid===true).length}/${trials.length} full streams timing/health-qualified · ${trials.filter(t=>t.qualified===true).length}/${trials.length} complete qualified · ${count(timing.count)}/${trials.length} timing valid (${timing.unknown} unknown) · ${count(native.count)} native failures (${native.unknown} unknown) · ${count(deadline.count)} deadline cancellations (${deadline.unknown} unknown) · ${count(startup.count)} startup failures (${startup.unknown} unknown) · ${count(driver.count)} benchmark driver faults (${driver.known}/${trials.length} known, ${driver.unknown} unknown)`:unavailable?"Selected evidence unavailable; no observations from another implementation are shown.":loading?"Loading selected evidence; previous observations cleared.":"Awaiting recorded outcomes.");
 const probes=probeAccounting(trials),p=probes.values;
 setText("probe-summary",trials.length?`Warmup + measurement probe accounting from ${probes.known}/${probes.total} trials: expected ${count(p.expected)} · injected ${count(p.injected)} · observed ${count(p.observed)} · missed injections ${count(p.missed)} · unobserved responses ${count(p.unobserved)} · deadline timeouts ${count(p.timeouts)}. Missing trial counters remain unknown; timeouts are part of unobserved responses.`:"Probe accounting unavailable until selected observations load.");
 const run=trials.filter(t=>t.outcome!=="NOT_RUN").length;
 setText("run-count",`${run} / ${m.planned_trials||80}`);setText("campaign-status",m.status||"Preparing");
 const commit=typeof m.commit==="string"?m.commit:null;
 setText("build-label",commit?`Build ${commit.slice(0,9)}`:"Source commit unavailable");
 setText("updated",m.generated_utc?`Updated ${new Date(m.generated_utc).toLocaleString()}`:"No evidence update available");
 setText("dataset-name",dataset.label);setText("dataset-commit",commit||"—");
 const adapter=m.native_adapter||DATA.plan?.native_adapter;
 setText("dataset-engine",typeof adapter?.id==="string"?adapter.id:trials.length&&state.implementation==="local"?"local rdpeudp recovery implementation":"—");
 setText("dataset-window",`own ${geometryText("own_log_window")} / peer ${geometryText("peer_log_window")}`);
 setText("dataset-storage",`${geometryText("sent_storage_slots")} physical / ${geometryText("usable_storage_packets")} usable slots`);
 const payloads=payloadCeilings(trials);
 setText("dataset-payload",`${rangeText(payloads)} bytes${payloads.length?` (${payloads.length}/${trials.length} recorded trials known)`:" (trial evidence unavailable)"}`);
 setText("provenance",commit?`${dataset.label}. Native source ${commit}. ${typeof m.scope==="string"?m.scope:"Native UDP2 transport"}. Frozen settings, source and all trial outcomes accompany the selected downloadable evidence.`:`${dataset.label}: source and trial evidence are unavailable until its dataset loads.`);
 for(const id of["download-json","provenance-download"])el(id).href=dataset.json;
 el("download-csv").href=dataset.csv;
}
function renderLegend(){el("legend").replaceChildren();for(const w of WINDOWS){const b=element("button",undefined,state.visible.has(w)?"":"off");b.setAttribute("aria-pressed",state.visible.has(w));const dot=element("i");dot.style.background=COLORS[w];b.append(dot,document.createTextNode(w+" packets"));b.onclick=()=>{state.visible.has(w)?state.visible.delete(w):state.visible.add(w);renderLegend();renderChart()};el("legend").append(b)}}
function svg(tag,attrs,text){const n=document.createElementNS("http://www.w3.org/2000/svg",tag);for(const[k,v]of Object.entries(attrs))n.setAttribute(k,v);if(text!==undefined)n.textContent=text;return n}
function renderChart(){const chart=el("chart");chart.replaceChildren();const points=[];for(const w of state.visible)for(const loss of LOSSES){const g=group(loss,w),m=metric(g,state.metric);if(finite(m.median))points.push({loss,w,g,m})}const width=Math.max(320,chart.clientWidth||1050),height=width<600?240:300;chart.setAttribute("viewBox",`0 0 ${width} ${height}`);const top=20,bottom=height-40,left=width<600?40:63,right=width-20,max=state.metric==="goodput_mbps"?10:Math.max(50,...points.map(p=>p.m.median*1.15));const x=l=>left+(right-left)*l/70,y=v=>bottom-(bottom-top)*v/max;for(let i=0;i<=4;i++){const val=max*i/4;chart.append(svg("line",{x1:left,x2:right,y1:y(val),y2:y(val),stroke:"#e9eee7","stroke-dasharray":"3 4"}),svg("text",{x:left-13,y:y(val)+4,"text-anchor":"end",fill:"#95a28f","font-size":11},fmt(val,max<=10?1:0)))}for(const loss of LOSSES)chart.append(svg("text",{x:x(loss),y:bottom+26,"text-anchor":"middle",fill:"#889980","font-size":11},loss+"%"));for(const w of state.visible){let current=[];function flush(){if(current.length>1)chart.append(svg("polyline",{points:current.map(p=>`${x(p.loss)},${y(p.m.median)}`).join(" "),fill:"none",stroke:COLORS[w],"stroke-width":2.3,"stroke-linejoin":"round"}));current=[]}for(const loss of LOSSES){const p=points.find(p=>p.loss===loss&&p.w===w);if(!p){flush();continue}current.push(p)}flush();for(const p of points.filter(p=>p.w===w)){const dot=svg("circle",{cx:x(p.loss),cy:y(p.m.median),r:4.4,fill:"white",stroke:COLORS[w],"stroke-width":2.2,tabindex:0,role:"button","aria-label":`${p.loss}% loss, ${w} packets: ${fmt(p.m.median)} ${METRICS[state.metric][1]}, N=${p.m.n}`});dot.append(svg("title",{},`${w} packets · ${p.loss}% loss\n${fmt(p.m.median)} ${METRICS[state.metric][1]} · ${p.m.n} trials`));dot.onclick=()=>selectCell(p.loss,w);dot.onkeydown=e=>{if(e.key==="Enter"||e.key===" "){e.preventDefault();selectCell(p.loss,w)}};chart.append(dot)}}el("chart-empty").classList.toggle("hidden",points.length>0);if(!points.length){const status=DATA.meta.status;el("chart-empty").querySelector("strong").textContent=status==="unavailable"?"Selected evidence unavailable":status==="loading"?"Loading selected implementation":status==="preparing"?"Measurements are being prepared":"No measurements in this cohort";el("chart-empty").querySelector("p").textContent=status==="unavailable"?(DATA.meta.load_error||"No measurements are shown."):status==="loading"?"Previous implementation observations have been cleared.":status==="preparing"?"Each curve requires recorded evidence.":"Review the trial ledger and retained failures."}setText("chart-caption",`${DATASETS[state.implementation].label} · ${METRICS[state.metric][0]}, ${METRICS[state.metric][1]} · ${state.cohort==="qualified"?"qualified complete":state.cohort==="integrity_valid"?"verified streams, valid timing":"diagnostic observations"}`);el("diagnostic-note").classList.toggle("hidden",state.cohort==="qualified")}
function renderMatrix(){const matrix=el("matrix");matrix.replaceChildren(element("div","Loss / limit","matrix-label header"));for(const w of WINDOWS)matrix.append(element("div",w+" packets","matrix-label header"));for(const loss of LOSSES){const label=element("div",undefined,"matrix-label loss"),inner=element("span",loss+"%");inner.append(element("small","random packet loss"));label.append(inner);matrix.append(label);for(const w of WINDOWS){const g=group(loss,w),m=metric(g,state.metric),b=element("button",undefined,"matrix-cell"+(state.loss===loss&&state.window===w?" active":"")+(g.run_count>0&&!g.qualified_count?" fail-cell":""));b.setAttribute("aria-pressed",state.loss===loss&&state.window===w);b.setAttribute("aria-label",`${loss}% loss, ${w} outstanding packets`);const v=element("div",fmt(m.median),"cell-num");v.append(element("span",finite(m.median)?METRICS[state.metric][1]:"","cell-unit"));const meta=element("div",undefined,"cell-meta"),status=element("span");status.append(element("i",undefined,"cell-status"+(g.run_count>0?(g.qualified_count>0?" good":" bad"):"")),document.createTextNode(g.run_count>0?`${g.qualified_count||0}/${g.planned_count||5} qualified`:DATA.meta.status==="unavailable"?"Unavailable":"Pending"));meta.append(status,element("span",g.fail_count>0?g.fail_count+" failed":g.run_count>0?"N="+(m.n||0):"5 trials"));b.append(v,meta);b.onclick=()=>selectCell(loss,w);matrix.append(b)}}}
function selectCell(loss,window){state.loss=loss;state.window=window;renderMatrix();renderDetail()}
function renderDetail(){
 const g=group(state.loss,state.window),gp=metric(g,"goodput_mbps"),p95=metric(g,"rtt_p95_ms"),p99=metric(g,"rtt_p99_ms"),p=g.probes||{};
 const cellTrials=DATA.trials.filter(t=>t.loss_percent===state.loss&&t.window_packets===state.window),startup=startupAccounting(cellTrials),driver=driverAccounting(cellTrials);
 const evidenceIssue=(g.artifact_invalid_count||0)+(g.incomplete_count||0)+(g.native_failure_unknown_count||0)+(g.deadline_unknown_count||0);
 setText("detail-name",`${state.loss}% loss · ${state.window} outstanding packets`);
 setText("detail-status",!g.run_count?(DATA.meta.status==="unavailable"?"Unavailable":DATA.meta.status==="loading"?"Loading":"Pending"):startup.count?"Startup failure retained":g.artifact_invalid_count?"Artifact invalid":g.incomplete_count?"Incomplete trials retained":g.native_failure_unknown_count||g.deadline_unknown_count?"Health evidence unknown":g.fail_count||g.harness_invalid_count?"Failures retained":g.run_count<g.planned_count?"In progress":"Recorded");
 el("detail-status").classList.toggle("fail",Boolean(evidenceIssue||g.fail_count||g.harness_invalid_count));
 addStat("goodput",fmt(gp.median),finite(gp.median)?"Mbit/s":"");
 setText("goodput-note",gp.n?`${gp.n} contributing trials · range ${fmt(gp.min)}–${fmt(gp.max)}`:"No complete verified streams in this cohort");
 addStat("tails",`${fmt(p95.median,1)} / ${fmt(p99.median,1)}`,finite(p99.median)?"ms":"");
 setText("tail-note",`Send attempt to observed response · selected cohort N=${p95.n||0}/${p99.n||0}`);
 addStat("missed",`${count(p.missed)} / ${count(p.unobserved)}`);
 setText("probe-note",finite(p.injected)?`All intact trials, warmup + measurement: ${count(p.observed)} / ${count(p.injected)} responses · ${count(p.timeouts)} timeouts`:"All intact trials, warmup + measurement: counts unavailable");
 addStat("peak",count(g.peak_inflight),finite(g.peak_inflight)?"packets":"");
 setText("peak-note",`All intact trials, lifetime incl. warmup/drain · limit ${state.window} · max sequence span ${count(g.peak_span)}`);
 setText("cell-accounting",`${g.run_count||0} of ${g.planned_count||5} trials recorded · ${g.qualified_count||0} qualified · ${g.integrity_count||0} full streams verified · artifact invalid ${count(g.artifact_invalid_count)} · unfinished attempts ${count(g.incomplete_count)}`);
 const a=g.actual_loss||{};
 setText("loss-observed",`All intact trials, random loss C→S / S→C: ${finite(a.c2s)?fmt(a.c2s*100,1)+"%":"—"} / ${finite(a.s2c)?fmt(a.s2c*100,1)+"%":"—"}`);
 setText("failure-accounting",g.run_count?`Native failures ${count(g.native_failure_count)} (${count(g.native_failure_unknown_count)} unknown) · reported benchmark driver faults ${count(driver.count)} (${driver.known}/${cellTrials.length} known, ${driver.unknown} unknown) · startup failures ${count(startup.count)} (${startup.unknown} unknown) · deadline cancellations ${count(g.deadline_count)} (${count(g.deadline_unknown_count)} unknown) · timing invalid ${count(g.harness_invalid_count)}`:"Failures: unavailable");
 const payloads=payloadCeilings(DATA.trials.filter(t=>t.loss_percent===state.loss&&t.window_packets===state.window));
 setText("cell-payload",`Actual DATA payload ceiling ${rangeText(payloads)} bytes · ${payloads.length} trials with recorded ceiling`);
 const q=g.queue_drops||{};
 setText("queue-accounting",`All intact trials, queue drops C→S / S→C: ${count(q.c2s)} / ${count(q.s2c)}`);
 el("cell-reasons").replaceChildren();
 const failures=DATA.trials.filter(t=>t.loss_percent===state.loss&&t.window_packets===state.window&&trialIssue(t));
 for(const t of failures)el("cell-reasons").append(element("p",`Seed ${t.seed}: ${trialStatus(t)}. ${trialReasons(t)||"Qualification did not pass; review the trial evidence."}`));
}
function plannedTrials(){if(DATA.meta.status==="unavailable"||DATA.meta.status==="loading")return [];const found=new Map(DATA.trials.map(t=>[`${t.loss_percent}/${t.window_packets}/${t.seed}`,t])),rows=[];for(const loss of LOSSES)for(const w of WINDOWS)for(const seed of(DATA.meta.seeds||[11,29,47,71,101]))rows.push(found.get(`${loss}/${w}/${seed}`)||{trial_id:`planned-${loss}-${w}-${seed}`,loss_percent:loss,window_packets:w,seed,outcome:"NOT_RUN",validity:"PENDING"});return rows}
function renderTrials(){
 const query=el("trial-search").value.toLowerCase(),loss=el("loss-filter").value,w=el("window-filter").value,outcome=el("outcome-filter").value;
 const rows=plannedTrials().filter(t=>(loss==="all"||String(t.loss_percent)===loss)&&(w==="all"||String(t.window_packets)===w)&&(outcome==="all"||t.outcome===outcome)&&`${t.trial_id} ${t.seed} ${trialStatus(t)} ${t.loss_percent} ${t.window_packets}`.toLowerCase().includes(query));
 setText("trial-total",DATA.meta.status==="unavailable"?"Evidence unavailable":DATA.meta.status==="loading"?"Loading evidence":`${rows.length} trials`);el("trial-body").replaceChildren();
 for(const t of rows){
  const tr=element("tr",undefined,trialIssue(t)?"failure-row":"");tr.dataset.id=t.trial_id;
  const integrity=t.validity==="INVALID_ARTIFACT"?"Unknown":t.integrity_verified===true?"Verified":t.integrity_verified===false?"Incomplete":"—";
  const values=[`${t.loss_percent}% / ${t.window_packets} packets`,String(t.seed),trialStatus(t),integrity,finite(t.goodput_mbps)?fmt(t.goodput_mbps)+" Mbit/s":"—",`${fmt(t.rtt_p95_ms,1)} / ${fmt(t.rtt_p99_ms,1)} ms · N=${count(t.observed_measurement_responses)}`,`${count(t.missed)} / ${count(t.unobserved)} / ${count(t.timeouts)}`,count(t.peak_inflight)];
  for(const val of values)tr.append(element("td",val));
  tr.onclick=()=>{const n=el("trial-reason");n.classList.remove("hidden");n.textContent=`${t.trial_id} · ${t.validity}. Observed measurement responses N=${count(t.observed_measurement_responses)}; missed / unobserved / timeouts (warmup + measurement) ${count(t.missed)} / ${count(t.unobserved)} / ${count(t.timeouts)}. ${trialReasons(t)||(t.outcome==="NOT_RUN"?"This condition is planned and has not run.":t.qualified?"No recorded failure reason.":"Qualification did not pass; review the trial evidence.")}${!t.qualified&&finite(t.goodput_mbps)?" Payload rate is diagnostic for this trial; qualification did not pass.":""}`};
  el("trial-body").append(tr);
 }
}
function renderNetwork(){
 const checks=DATA.network_checks||{},samples=checks.samples||[];
 setText("network-status",checks.status||"Pending");el("network-body").replaceChildren();
 if(!samples.length){const tr=element("tr"),td=element("td",DATA.meta.status==="unavailable"?"Network evidence unavailable for the selected implementation.":"Independent profile verification is being prepared.");td.colSpan=6;tr.append(td);el("network-body").append(tr)}
 for(const s of samples){
  const tr=element("tr");
  for(const val of[`${s.loss_percent??"—"}%`,`${s.direction||"—"} / ${s.mode||"CRC train"}`,`${count(s.offered)} / ${count(s.received)}`,finite(s.actual_loss)?fmt(s.actual_loss*100,2)+"%":"—",finite(s.received_rate_mbps)?fmt(s.received_rate_mbps)+" Mbit/s":"—",s.outcome||"Unknown"])tr.append(element("td",val));
  el("network-body").append(tr);
  const detailRow=element("tr"),td=element("td"),details=element("details"),criteria=Array.isArray(s.criteria)?s.criteria:[],failed=criteria.filter(c=>c.pass===false);
  td.colSpan=6;td.style.whiteSpace="normal";
  details.append(element("summary",`Queue drops ${count(s.queue_drops)} · socket errors ${count(s.socket_errors)} · pending ${count(s.pending)} · ${criteria.length?failed.length+" failed criteria":"criteria unavailable"} — details`));
  for(const criterion of failed){
   details.append(element("p",`Failed: ${criterion.criterion||"Unnamed criterion"}`));
   if(criterion.detail!==undefined&&criterion.detail!==null){const value=typeof criterion.detail==="string"?criterion.detail:JSON.stringify(criterion.detail);const text=value.length>1000?value.slice(0,1000)+"… (full detail in JSON)":value;const note=element("p",text);note.style.overflowWrap="anywhere";details.append(note)}
  }
  if(criteria.length&&!failed.length)details.append(element("p","All recorded criteria passed."));
  td.append(details);detailRow.append(td);el("network-body").append(detailRow);
 }
}
function render(){renderHeader();renderLegend();renderChart();renderMatrix();renderDetail();renderTrials();renderNetwork()}
document.querySelectorAll("[data-view]").forEach(b=>b.onclick=()=>{document.querySelectorAll("[data-view]").forEach(n=>n.classList.toggle("active",n===b));document.querySelectorAll(".view").forEach(n=>n.classList.toggle("active",n.id===b.dataset.view));location.hash=b.dataset.view});document.querySelectorAll("[data-metric]").forEach(b=>b.onclick=()=>{state.metric=b.dataset.metric;document.querySelectorAll("[data-metric]").forEach(n=>n.classList.toggle("selected",n===b));renderChart();renderMatrix()});el("cohort").onchange=()=>{state.cohort=el("cohort").value;renderChart();renderMatrix();renderDetail()};for(const id of["trial-search","loss-filter","window-filter","outcome-filter"])el(id).addEventListener(id==="trial-search"?"input":"change",renderTrials);
async function loadDataset(implementation){
 if(!Object.prototype.hasOwnProperty.call(DATASETS,implementation))return;
 state.implementation=implementation;el("implementation").value=implementation;
 const request=++datasetRequest,dataset=DATASETS[implementation];
 DATA={meta:{status:"loading",planned_trials:80},groups:[],trials:[],network_checks:{status:"pending",samples:[]}};
 el("trial-reason").classList.add("hidden");el("trial-reason").textContent="";render();
 try{
  const response=await fetch(dataset.json,{cache:"no-store"});
  if(!response.ok)throw new Error(`${dataset.label}: evidence file unavailable (${response.status}).`);
  const data=await response.json();
  if(!data||!data.meta||!Array.isArray(data.groups)||!Array.isArray(data.trials))throw new Error("Unsupported evidence schema.");
  if(request!==datasetRequest)return;
  DATA=data;render();
 }catch(error){
  if(request!==datasetRequest)return;
  DATA={meta:{status:"unavailable",planned_trials:80,load_error:typeof error.message==="string"?error.message:"Evidence could not be loaded."},groups:[],trials:[],network_checks:{status:"unavailable",samples:[]}};
  render();
 }
}
el("implementation").onchange=()=>loadDataset(el("implementation").value);
window.addEventListener("resize",renderChart);
const view=location.hash.slice(1);if(["overview","network","trials","method"].includes(view))document.querySelector(`[data-view="${view}"]`).click();
loadDataset("pr13608");
