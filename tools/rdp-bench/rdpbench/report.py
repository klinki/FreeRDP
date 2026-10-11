"""Append-only dashboard store and conservative paired comparisons; no fabricated sample runs."""
from __future__ import annotations
import csv,json,random,re,sqlite3,statistics
from pathlib import Path
from .common import read_json,audit,write_json

def bootstrap_pair(a,b,reps=10000):
    if len(a)!=len(b) or not a:raise ValueError('paired nonempty data required')
    delta=[y-x for x,y in zip(a,b)];rng=random.Random(20261011)
    samples=sorted(statistics.median(rng.choices(delta,k=len(delta))) for _ in range(reps))
    return {'pairs':len(delta),'baseline_median':statistics.median(a),'candidate_median':statistics.median(b),
            'median_delta':statistics.median(delta),'ci95_delta':[samples[int(.025*reps)],samples[min(reps-1,int(.975*reps))]],
            'method':'paired-run median difference percentile bootstrap; exploratory for small N'}

def collect(root):
    out=[]
    root=Path(root)
    folders={p.parent for name in ('summary.json','manifest.json') for p in root.rglob(name)}
    # A partially written trial or deleted summary must not disappear from history.
    for campaign in root.rglob('campaign.json'):
        for trial in read_json(campaign).get('trials',[]):
            name=trial.get('trial_id','')
            if isinstance(name,str) and re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*',name):folders.add(campaign.parent/name)
    for folder in sorted(folders):
        path=folder/'summary.json'
        try:
            errors=audit(path.parent)
            r=read_json(path)
            if errors:r['validity']='INVALID_ARTIFACT';r['reasons']=errors
            out.append(r)
        except Exception as e:out.append({'trial_id':path.parent.name,'validity':'INVALID_ARTIFACT','reasons':[repr(e)],'metrics':{}})
    return out

def compare(root,baseline,candidate,metric,out):
    if baseline==candidate:raise ValueError('baseline and candidate must differ')
    data=collect(root);groups={};excluded=[]
    for r in data:
        if r.get('variant') not in (baseline,candidate):continue
        if r.get('validity')!='VALIDATED_LAB' or r.get('sut_outcome')!='COMPLETE' or r.get('metrics',{}).get(metric) is None:
            excluded.append({'trial':r['trial_id'],'reason':r.get('validity'),'outcome':r.get('sut_outcome')});continue
        key=(r['case_id'],r['comparison_key'],r['seed'])
        if r['variant'] in groups.setdefault(key,{}):raise ValueError('duplicate seed pair: specify a single campaign root')
        groups[key][r['variant']]=(r['metrics'][metric],r['trial_id'])
    bycase={}
    for key,g in groups.items():
        if baseline in g and candidate in g:
            a,b=bycase.setdefault(key[:2],([],[]));a.append(g[baseline][0]);b.append(g[candidate][0])
        else:
            excluded.extend({'trial':value[1],'reason':'no complete matching seed/configuration pair'} for value in g.values())
    results=[]
    for key,(a,b) in bycase.items():
        r=bootstrap_pair(a,b);r.update(case_id=key[0],comparison_key=key[1],metric=metric)
        practical=max(5 if metric.endswith(('.p95','.p99','.p50','.max')) and '_ms' in metric else 0,abs(r['baseline_median'])*.05)
        lo,hi=r['ci95_delta'];r['practical_delta']=practical
        r['decision']='INSUFFICIENT_REPETITIONS' if len(a)<5 else 'CHANGE_DETECTED' if lo>practical or hi<-practical else 'NO_CLEAR_PRACTICAL_CHANGE'
        results.append(r)
    result={'baseline':baseline,'candidate':candidate,'metric':metric,'comparisons':results,'excluded':excluded,
            'warning':'Any failed candidate run must be reviewed as a reliability regression, not discarded because its latency is unavailable.'}
    write_json(out,result);return result

def dashboard(root,out):
    rows=collect(root);out=Path(out);out.mkdir(parents=True,exist_ok=True)
    db=sqlite3.connect(out/'history.sqlite')
    db.execute('CREATE TABLE IF NOT EXISTS trials (id TEXT PRIMARY KEY, utc TEXT, suite TEXT, case_id TEXT, variant TEXT, validity TEXT, outcome TEXT, data TEXT)')
    for r in rows:
        ident=r['trial_id'];encoded=json.dumps(r,sort_keys=True)
        old=db.execute('SELECT data FROM trials WHERE id=?',(ident,)).fetchone()
        if old and old[0]!=encoded:raise ValueError(f'refusing history mutation: {ident}')
        db.execute('INSERT OR IGNORE INTO trials VALUES(?,?,?,?,?,?,?,?)',(ident,r.get('utc'),r.get('suite'),r.get('case_id'),r.get('variant'),r.get('validity'),r.get('sut_outcome'),encoded))
    db.commit();allrows=[json.loads(r[0]) for r in db.execute('SELECT data FROM trials ORDER BY utc,id')];db.close()
    write_json(out/'data.json',allrows)
    # Long-form CSV imports directly into ordinary BI/time-series pipelines.
    with open(out/'metrics.csv','w',newline='',encoding='utf-8') as f:
        w=csv.writer(f);w.writerow(['trial_id','utc','suite','case_id','variant','commit','binary_sha256','comparison_key','validity','sut_outcome','metric','value'])
        for r in allrows:
            for m,v in r.get('metrics',{}).items():w.writerow([r.get(k) for k in ['trial_id','utc','suite','case_id','variant','commit','binary_sha256','comparison_key','validity','sut_outcome']]+[m,v])
    payload=json.dumps(allrows).replace('<','\\u003c')
    doc='''<!doctype html><meta charset="utf-8"><title>RDP benchmark history</title>
<style>body{font:15px system-ui;max-width:1400px;margin:30px auto;padding:0 20px}table{border-collapse:collapse;width:100%}td,th{border:1px solid #ccc;padding:8px;text-align:left}th{background:#eee}select{padding:8px;margin:8px}svg{border:1px solid #ddd;width:100%;height:280px}.warn{background:#fff0dc;padding:15px}pre{white-space:pre-wrap}</style>
<h1>RDP benchmark history</h1><p class="warn">Each point is a whole trial, not a one-second sample. PROVISIONAL, INVALID and failed runs remain visible. Select one case and comparable lab/configuration before interpreting a trend. Unknown metrics stay missing.</p>
<label>Case <select id="case"></select></label><label>Comparable configuration <select id="group"></select></label><label>Metric <select id="metric"></select></label><svg id="plot" viewBox="0 0 1000 280"></svg>
<table><thead><tr><th>UTC</th><th>Case</th><th>Variant / commit</th><th>Validity</th><th>SUT outcome</th><th>Value</th><th>Reasons</th></tr></thead><tbody id="body"></tbody></table>
<script>const DATA=__DATA__;const E=id=>document.getElementById(id);const unique=x=>[...new Set(x)].sort();
for(const c of unique(DATA.map(r=>r.case_id||'?')))E('case').add(new Option(c,c));
for(const m of unique(DATA.flatMap(r=>Object.keys(r.metrics||{}))))E('metric').add(new Option(m,m));
function groups(){E('group').replaceChildren();for(const k of unique(DATA.filter(r=>r.case_id===E('case').value).map(r=>r.comparison_key||'unknown')))E('group').add(new Option(k.slice(0,16),k));}
function draw(){const data=DATA.filter(r=>r.case_id===E('case').value&&(r.comparison_key||'unknown')===E('group').value),metric=E('metric').value;E('body').replaceChildren();E('plot').replaceChildren();
const points=data.map((r,i)=>[i,r.metrics?.[metric],r]).filter(p=>Number.isFinite(p[1]));const max=Math.max(1,...points.map(p=>p[1]));
function svg(tag,attrs,text){const e=document.createElementNS('http://www.w3.org/2000/svg',tag);for(const[k,v]of Object.entries(attrs))e.setAttribute(k,v);if(text!==undefined)e.textContent=text;E('plot').appendChild(e);return e}
for(let j=0;j<=4;j++){const y=250-j*55;svg('line',{x1:50,x2:980,y1:y,y2:y,stroke:'#ddd'});svg('text',{x:2,y:y-3,'font-size':11},(max*j/4).toFixed(2))}
svg('text',{x:55,y:275,'font-size':11},data[0]?.utc||'No trials');svg('text',{x:745,y:275,'font-size':11},data.at(-1)?.utc||'');
for(const [i,y,r] of points){const c=document.createElementNS('http://www.w3.org/2000/svg','circle');c.setAttribute('cx',55+i*900/Math.max(1,data.length-1));c.setAttribute('cy',250-y/max*220);c.setAttribute('r',5);c.setAttribute('fill',r.validity==='VALIDATED_LAB'&&r.sut_outcome==='COMPLETE'?'#166b8e':'#bf681a');const t=document.createElementNS(c.namespaceURI,'title');t.textContent=`${r.variant}: ${y} | ${r.validity} | ${r.comparison_key}`;c.appendChild(t);E('plot').appendChild(c)}
for(const r of data){const tr=document.createElement('tr');for(const value of [r.utc,r.case_id,`${r.variant} / ${(r.commit||'unknown').slice(0,12)}`,r.validity,r.sut_outcome,r.metrics?.[metric]??'unavailable',JSON.stringify([...(r.reasons||[]),...(r.sut_failures||[])])]){const td=document.createElement('td');td.textContent=value??'';tr.appendChild(td)}E('body').appendChild(tr)}}
E('case').onchange=()=>{groups();draw()};E('group').onchange=draw;E('metric').onchange=draw;groups();draw();</script>'''.replace('__DATA__',payload)
    (out/'index.html').write_text(doc,encoding='utf-8')
    return len(allrows)
