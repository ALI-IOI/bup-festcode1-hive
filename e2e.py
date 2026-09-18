import json, sys, urllib.request
BASE = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8080"
TOL = 0.01
d = json.load(open('/mnt/user-data/uploads/BUP_CSE_FEST_2026_Preli_Public_Sample_Cases.json'))

def post(body):
    r = urllib.request.Request(BASE+"/optimize-energy",
        data=json.dumps(body).encode(), headers={"Content-Type":"application/json"})
    with urllib.request.urlopen(r, timeout=35) as f:
        return json.load(f)

TOP = ["scenario_id","directive_interpretation","hourly_plan","total_grid_kwh",
       "total_cost_bdt","peak_grid_kwh","plan_summary"]
HP  = ["hour","grid_kwh","solar_used_kwh","battery_action","battery_kwh","battery_energy_after_kwh"]
DI  = ["note_index","applies","directive_type","structured_adjustment","explanation"]

def check(case, out):
    scn, errs = case['input'], []
    for k in TOP:
        if k not in out: errs.append(f"missing top-level {k}")
    if errs: return errs
    if out['scenario_id'] != scn['scenario_id']: errs.append("scenario_id not echoed")
    di = out['directive_interpretation']
    if len(di) != len(scn['operator_notes']): errs.append("interpretation count != note count")
    for i,e in enumerate(di):
        for k in DI:
            if k not in e: errs.append(f"di[{i}] missing {k}")
        if e.get('note_index') != i: errs.append(f"di[{i}] note_index out of order")
        if e.get('directive_type') == 'no_op':
            if e.get('applies') is not False: errs.append(f"di[{i}] no_op applies must be false")
            if e.get('structured_adjustment') is not None: errs.append(f"di[{i}] no_op adj must be null")
        else:
            if e.get('applies') is not True: errs.append(f"di[{i}] non-no_op applies must be true")
            hs = (e.get('structured_adjustment') or {}).get('hours', [])
            if hs != sorted(set(hs)) or any(not isinstance(h,int) or h<0 or h>23 for h in hs):
                errs.append(f"di[{i}] hours not unique ascending ints 0-23")
    # replay the plan using the team's OWN reported directives
    hrs = sorted(scn['hours'], key=lambda x:x['hour']); b = scn['battery']
    eff=[h['solar_kwh'] for h in hrs]; lo=[b['minimum_energy_kwh']]*24
    cc=[b['max_charge_kwh_per_hour']]*24; dd=[b['max_discharge_kwh_per_hour']]*24
    gc=[float('inf')]*24
    for e in di:
        a = e.get('structured_adjustment') or {}
        for h in a.get('hours',[]):
            t=e['directive_type']
            if   t=='solar_reduction':         eff[h]*=a['factor']
            elif t=='minimum_battery_reserve': lo[h]=max(lo[h],a['minimum_energy_kwh'])
            elif t=='no_charge_window':        cc[h]=0
            elif t=='no_discharge_window':     dd[h]=0
            elif t=='max_grid_window':         gc[h]=min(gc[h],a['max_grid_kwh'])
    plan = sorted(out['hourly_plan'], key=lambda p:p['hour'])
    if [p['hour'] for p in plan] != list(range(24)): errs.append("hourly_plan hours != 0..23")
    for p in plan:
        for k in HP:
            if k not in p: errs.append(f"h{p.get('hour')} missing {k}")
    if errs: return errs
    E=b['initial_energy_kwh']; tg=tc=pk=0
    for p in plan:
        h=p['hour']
        ch = p['battery_kwh'] if p['battery_action']=='charge' else 0
        di_=p['battery_kwh'] if p['battery_action']=='discharge' else 0
        if p['battery_action'] not in ('charge','discharge','idle'): errs.append(f"h{h} bad action")
        if p['battery_action']=='idle' and abs(p['battery_kwh'])>TOL: errs.append(f"h{h} idle w/ kwh")
        if min(p['grid_kwh'],p['solar_used_kwh'],p['battery_kwh'])<-TOL: errs.append(f"h{h} negative")
        if p['solar_used_kwh']>eff[h]+TOL: errs.append(f"h{h} solar over effective")
        if p['grid_kwh']>gc[h]+TOL: errs.append(f"h{h} over grid cap")
        if ch>cc[h]+TOL: errs.append(f"h{h} charge over cap/window")
        if di_>dd[h]+TOL: errs.append(f"h{h} discharge over cap/window")
        bal=p['grid_kwh']+p['solar_used_kwh']+di_-(hrs[h]['demand_kwh']+ch)
        if abs(bal)>TOL: errs.append(f"h{h} balance off {bal:.4f}")
        E+=ch-di_
        if abs(E-p['battery_energy_after_kwh'])>TOL: errs.append(f"h{h} E mismatch")
        if E<lo[h]-TOL or E>b['capacity_kwh']+TOL: errs.append(f"h{h} E out of bounds")
        tg+=p['grid_kwh']; tc+=p['grid_kwh']*hrs[h]['tariff_bdt_per_kwh']; pk=max(pk,p['grid_kwh'])
    if abs(E-b['initial_energy_kwh'])>TOL: errs.append("end-of-day neutrality violated")
    for k,v in (('total_grid_kwh',tg),('total_cost_bdt',tc),('peak_grid_kwh',pk)):
        if abs(out[k]-v)>TOL: errs.append(f"{k} {out[k]} != recalculated {v:.4f}")
    return errs

ok=0
for c in d['cases']:
    try: out = post(c['input'])
    except Exception as ex: print(f"{c['id']}  REQUEST FAILED: {ex}"); continue
    errs = check(c, out)
    types = [e['directive_type'] for e in out['directive_interpretation']]
    exp   = [e['directive_type'] for e in c['expected_output']['directive_interpretation']]
    match = "OK " if types==exp else "DIFF"
    print(f"{c['id']}  schema={'PASS' if not errs else 'FAIL'}  cost={out['total_cost_bdt']:>9.2f} "
          f"(ref {c['expected_output']['total_cost_bdt']})  types[{match}] {types}")
    for e in errs[:3]: print("      !", e)
    ok += not errs
print(f"\n{ok}/{len(d['cases'])} responses schema-valid and self-consistent")
