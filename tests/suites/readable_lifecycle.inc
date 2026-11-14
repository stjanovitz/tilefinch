/* Real bootstrap and owner-loop lifecycle checks; no network or timed races. */
static const char stream_owner_controls[] =
    "(async()=>{const check=(v,m)=>{if(!v)throw Error(m)};let c;"
    "const s=new ReadableStream({start(v){c=v}});for(let i=0;i<64;i++){"
    "const r=s.getReader(),old=r.closed.catch(e=>e);r.releaseLock();"
    "check(await old instanceof TypeError,'release notification')}"
    "const r=s.getReader(),reason={};c.error(reason);"
    "check(await r.closed.catch(e=>e)===reason,'current reader error');r.releaseLock();"
    "let finish,entered=false;const t=new WritableStream({write(){entered=true;"
    "return new Promise((_,reject)=>finish=reject)}}),a=t.getWriter(),write=a.write(1).catch(e=>e);"
    "for(let i=0;i<16&&!entered;i++)await Promise.resolve();check(entered,'write admission');"
    "a.releaseLock();const b=t.getWriter(),closed=b.closed.catch(e=>e);finish(reason);"
    "check(await write===reason&&await closed===reason,'successor write error');"
    "check(await b.ready.catch(e=>e)===reason,'successor readiness');b.releaseLock();"
    "const u=new ReadableStream(),bad={get preventCancel(){throw reason}};"
    "let error;try{u.values(bad)}catch(e){error=e}check(error===reason&&!u.locked,'option rollback');"
    "const it=u.values(null);await it.return();check(!u.locked,'null options');"
    "pocSummary='OK'})().catch(e=>pocSummary=String(e));";

static const char iterator_queue_bound[] =
    "(async()=>{let c;const s=new ReadableStream({start(v){c=v}}),it=s.values(),reads=[];"
    "for(let i=0;i<64;i++)reads.push(it.next());const refused=await it.next().catch(e=>e);"
    "if(!(refused instanceof RangeError))throw Error('iterator admission unbounded');"
    "c.close();for(const read of reads)if(!(await read).done)throw Error('queued EOF');"
    "if(s.locked||!(await it.next()).done)throw Error('iterator did not retire capacity');"
    "pocSummary='OK'})().catch(e=>pocSummary=String(e));";

static const char reader_release_handoff[] =
    "(async()=>{let c,released=false;const s=new ReadableStream({start(v){c=v}}),"
    "a=s.getReader(),old=a.closed.catch(e=>{released=e instanceof TypeError});"
    "const read=a.read().catch(e=>e);a.releaseLock();const b=s.getReader();"
    "let done=false;b.closed.then(()=>done=true);await Promise.resolve();"
    "if(!released||done||!(await read instanceof TypeError))throw Error('reader owner handoff');"
    "a.releaseLock();if(!s.locked)throw Error('old owner unlocked successor');"
    "c.enqueue(9);c.close();if((await b.read()).value!==9)throw Error('successor data');"
    "await b.closed;await old;b.releaseLock();pocSummary='OK'"
    "})().catch(e=>pocSummary=String(e));";

static const char reader_terminal_release[] =
    "(async()=>{const s=new ReadableStream({start(c){c.close()}}),r=s.getReader(),"
    "old=r.closed;await old;r.releaseLock();if(r.closed===old||"
    "!(await r.closed.catch(e=>e) instanceof TypeError))throw Error('closed reader release');"
    "await old;const reason={},t=new ReadableStream({start(c){c.error(reason)}}),"
    "a=t.getReader();if(await a.closed.catch(e=>e)!==reason)throw Error('initial error');"
    "a.releaseLock();if(!(await a.closed.catch(e=>e) instanceof TypeError))"
    "throw Error('errored reader release');const b=t.getReader();"
    "if(await b.closed.catch(e=>e)!==reason)throw Error('successor error');"
    "b.releaseLock();pocSummary='OK'})().catch(e=>pocSummary=String(e));";

static const char writer_release_handoff[] =
    "(async()=>{let finish,entered=false,released=false;const s=new WritableStream({"
    "write(){entered=true;return new Promise(r=>finish=r)}}),a=s.getWriter();"
    "a.closed.catch(e=>released=e instanceof TypeError);const write=a.write(1);"
    "for(let i=0;i<16&&!entered;i++)await Promise.resolve();"
    "if(!entered)throw Error('write never entered');a.releaseLock();const b=s.getWriter();"
    "let closed=false;b.closed.then(()=>closed=true);await Promise.resolve();"
    "if(!released||closed||!(await a.ready.catch(e=>e) instanceof TypeError))"
    "throw Error('writer owner handoff');a.releaseLock();if(!s.locked)"
    "throw Error('old writer unlocked successor');finish();await write;await b.close();"
    "await b.closed;b.releaseLock();pocSummary='OK'})().catch(e=>pocSummary=String(e));";

static const char writer_terminal_release[] =
    "(async()=>{const s=new WritableStream(),a=s.getWriter();await a.close();"
    "const old=a.closed;await old;a.releaseLock();if(a.closed===old||"
    "!(await a.closed.catch(e=>e) instanceof TypeError))throw Error('closed writer release');"
    "const reason={},t=new WritableStream({write(){throw reason}}),b=t.getWriter();"
    "const closed=b.closed.catch(e=>e);if(await b.write(1).catch(e=>e)!==reason||"
    "await closed!==reason)throw Error('writer error');b.releaseLock();"
    "const c=t.getWriter();if(await c.closed.catch(e=>e)!==reason||"
    "await c.ready.catch(e=>e)!==reason)throw Error('errored writer reacquire');"
    "c.releaseLock();pocSummary='OK'})().catch(e=>pocSummary=String(e));";

static const char iterator_read_failure[] =
    "(async()=>{const reason={},s=new ReadableStream({pull(c){c.error(reason)}}),"
    "it=s.values();if(await it.next().catch(e=>e)!==reason||s.locked)"
    "throw Error('failed iterator retained lock');if(!(await it.next()).done)"
    "throw Error('failed iterator resurrected');pocSummary='OK'"
    "})().catch(e=>pocSummary=String(e));";

static const char iterator_cancel_failure[] =
    "(async()=>{const reason={},s=new ReadableStream({cancel(){throw reason}}),"
    "it=s.values();if(await it.return().catch(e=>e)!==reason||s.locked)"
    "throw Error('failed cancellation retained lock');if(!(await it.next()).done)"
    "throw Error('cancelled iterator resurrected');pocSummary='OK'"
    "})().catch(e=>pocSummary=String(e));";

static const char iterator_terminal_repeat[] =
    "(async()=>{const s=new ReadableStream({start(c){c.close()}}),it=s.values();"
    "if(!(await it.next()).done||!(await it.next()).done||s.locked)"
    "throw Error('iterator EOF not stable');const end=await it.return(17);"
    "if(!end.done||end.value!==17)throw Error('iterator return value');"
    "pocSummary='OK'})().catch(e=>pocSummary=String(e));";

static const char iterator_return_order[] =
    "(async()=>{let c,cancelled=false,reason,finish;const s=new ReadableStream({"
    "start(v){c=v},cancel(v){cancelled=true;reason=v;return new Promise(r=>finish=r)}}),"
    "it=s.values(),read=it.next(),end=it.return(23);await Promise.resolve();"
    "if(cancelled)throw Error('return overtook pending next');c.enqueue(7);"
    "if((await read).value!==7)throw Error('accepted read lost');"
    "for(let i=0;i<16&&!cancelled;i++)await Promise.resolve();if(!cancelled||s.locked||reason!==23)"
    "throw Error('cancel handoff waits for source');finish();const result=await end;"
    "if(!result.done||result.value!==23)throw Error('return result');"
    "pocSummary='OK'})().catch(e=>pocSummary=String(e));";

static const char iterator_prevent_cancel[] =
    "(async()=>{let calls=0,cancels=0;const options={get preventCancel(){calls++;return true}},"
    "s=new ReadableStream({start(c){c.enqueue(4);c.close()},cancel(){cancels++}}),"
    "it=s.values(options);if(calls!==1)throw Error('options not captured on admission');"
    "if((await it.return(3)).value!==3||s.locked||cancels||calls!==1)"
    "throw Error('preventCancel release');const r=s.getReader();"
    "if((await r.read()).value!==4)throw Error('preventCancel lost queued data');"
    "r.releaseLock();pocSummary='OK'})().catch(e=>pocSummary=String(e));";

static const char readable_start_handoff[] =
    "(async()=>{const check=(v,m)=>{if(!v)throw Error(m)};let ready,pulls=0;"
    "const source=new ReadableStream({start(){return new Promise(r=>ready=r)},"
    "pull(c){pulls++;c.enqueue('ready');c.close()}}),reader=source.getReader(),"
    "read=reader.read();check(pulls===0,'pull entered pending start');await Promise.resolve();"
    "check(pulls===0,'pull entered before start settled');ready();"
    "check((await read).value==='ready'&&pulls===1,'start did not release one pull');"
    "check((await reader.read()).done,'start stream EOF');await reader.closed;"
    "pocSummary='OK'})().catch(e=>pocSummary=String(e));";

static const char readable_start_rejection[] =
    "(async()=>{let fail,pulls=0;const reason={},s=new ReadableStream({"
    "start(){return new Promise((_,r)=>fail=r)},pull(){pulls++}}),r=s.getReader(),"
    "closed=r.closed.catch(e=>e),read=r.read().catch(e=>e);fail(reason);"
    "if(await read!==reason||await closed!==reason||pulls!==0)"
    "throw Error('rejected startup entered source');pocSummary='OK'"
    "})().catch(e=>pocSummary=String(e));";

static const char readable_zero_demand[] =
    "(async()=>{let pulls=0;const s=new ReadableStream({pull(c){pulls++;c.enqueue(7)}},"
    "{highWaterMark:0});await Promise.resolve();await Promise.resolve();"
    "if(pulls)throw Error('zero demand pulled');const r=s.getReader();"
    "if((await r.read()).value!==7||pulls!==1)throw Error('explicit demand lost');"
    "await r.cancel();pocSummary='OK'})().catch(e=>pocSummary=String(e));";

static const char readable_close_drain[] =
    "(async()=>{let c,closed=false;const s=new ReadableStream({start(v){c=v;"
    "v.enqueue('first');v.enqueue('last');v.close()}}),r=s.getReader();"
    "r.closed.then(()=>closed=true);await Promise.resolve();"
    "if(closed||c.desiredSize!==-1)throw Error('closed before draining');"
    "if((await r.read()).value!=='first'||closed)throw Error('first chunk handoff');"
    "if((await r.read()).value!=='last')throw Error('last chunk lost');"
    "await r.closed;if(!closed||!(await r.read()).done)throw Error('drain not terminal');"
    "let refused=false;try{c.close()}catch(e){refused=e instanceof TypeError}"
    "if(!refused)throw Error('duplicate close accepted');pocSummary='OK'"
    "})().catch(e=>pocSummary=String(e));";

static const char readable_close_cancel[] =
    "(async()=>{let c,cancels=0;const reason={},s=new ReadableStream({start(v){c=v;"
    "v.enqueue('discard');v.close()},cancel(r){if(r!==reason)throw Error('reason');"
    "cancels++}}),r=s.getReader();await r.cancel(reason);await r.cancel();"
    "if(cancels!==1||!(await r.read()).done)throw Error('closing cancellation lost');"
    "pocSummary='OK'})().catch(e=>pocSummary=String(e));";

static const char readable_close_error[] =
    "(async()=>{let c;const reason={},s=new ReadableStream({start(v){c=v;"
    "v.enqueue('discard');v.close()}}),r=s.getReader(),closed=r.closed.catch(e=>e);"
    "c.error(reason);if(await r.read().catch(e=>e)!==reason||await closed!==reason)"
    "throw Error('closing error ignored');pocSummary='OK'})().catch(e=>pocSummary=String(e));";

static const char readable_empty_cancel[] =
    "(async()=>{const s=new ReadableStream(),r=s.getReader();let settled=false;"
    "const read=r.read().then(v=>{settled=true;return v});await Promise.resolve();"
    "if(settled)throw Error('empty constructor closed itself');await r.cancel();"
    "if(!(await read).done)throw Error('empty cancellation');pocSummary='OK'"
    "})().catch(e=>pocSummary=String(e));";

static const char readable_cancel_start[] =
    "(async()=>{let ready,pulls=0,cancels=0;const s=new ReadableStream({"
    "start(){return new Promise(r=>ready=r)},pull(){pulls++},cancel(){cancels++}}),"
    "r=s.getReader(),read=r.read();await r.cancel();ready();await Promise.resolve();"
    "if(!(await read).done||pulls||cancels!==1)throw Error('cancel resurrected startup');"
    "pocSummary='OK'})().catch(e=>pocSummary=String(e));";

static const char pipe_locked_admission[] =
    "(async()=>{const s=new ReadableStream({start(){}}),owner=s.getReader(),"
    "d=new WritableStream();let p;try{p=s.pipeTo(d)}catch(e){"
    "throw Error('pipe admission threw synchronously')}"
    "if(!(await p.catch(e=>e) instanceof TypeError)||d.locked)"
    "throw Error('refused pipe retained destination');owner.releaseLock();"
    "const next=s.getReader();await next.cancel();next.releaseLock();"
    "await s.pipeTo(d);if(s.locked||d.locked)throw Error('retry retained locks');"
    "pocSummary='OK'})().catch(e=>pocSummary=String(e));";

static const char pipe_failed_cleanup[] =
    "(async()=>{let cancels=0;const reason={},cleanup={},abort=new AbortController(),"
    "s=new ReadableStream({start(){},cancel(r){if(r!==reason)throw Error('cancel reason');"
    "cancels++}}),d=new WritableStream({abort(){throw cleanup}});"
    "d.getWriter().releaseLock();const pipe=s.pipeTo(d,{signal:abort.signal}).catch(e=>e);"
    "abort.abort(reason);if(await pipe!==cleanup||cancels!==1||s.locked||d.locked)"
    "throw Error('failed sink cleanup skipped source cancellation');"
    "pocSummary='OK'})().catch(e=>pocSummary=String(e));";

static const char pipe_destination_failure[] =
    "(async()=>{let cancels=0;const reason={},s=new ReadableStream({start(){},"
    "cancel(r){if(r!==reason)throw Error('cancel reason');cancels++}}),"
    "d=new WritableStream({start(){return Promise.reject(reason)}});"
    "const error=await s.pipeTo(d).catch(e=>e);if(error!==reason||cancels!==1||"
    "s.locked||d.locked)throw Error('destination failure handoff');"
    "pocSummary='OK'})().catch(e=>pocSummary=String(e));";

static const char pipe_lifecycle_controls[] =
    "(async()=>{const check=(v,m)=>{if(!v)throw Error(m)};"
    "{const s=new ReadableStream({start(){}}),d=new WritableStream(),owner=d.getWriter();"
    "check(await s.pipeTo(d).catch(e=>e) instanceof TypeError&&!s.locked,"
    "'locked destination acquired source');owner.releaseLock();await s.cancel()}"
    "{let next=0,total=0,closed=0;const s=new ReadableStream({pull(c){"
    "c.enqueue(++next);if(next===128)c.close()}}),d=new WritableStream({"
    "write(n){return Promise.resolve().then(()=>total+=n)},close(){closed++}});"
    "await s.pipeTo(d);check(total===8256&&closed===1&&!s.locked&&!d.locked,"
    "'ordinary pipeline');check((await s.getReader().read()).done,'ordinary EOF')}"
    "{let c,cancels=0,aborts=0;const a=new AbortController(),reason={},"
    "s=new ReadableStream({start(v){c=v},cancel(){cancels++}}),"
    "d=new WritableStream({abort(){aborts++}}),p=s.pipeTo(d,{signal:a.signal,"
    "preventAbort:true,preventCancel:true}).catch(e=>e);a.abort(reason);"
    "check(await p===reason&&!s.locked&&!d.locked&&cancels===0&&aborts===0,"
    "'prevent flags');c.enqueue(3);c.close();const r=s.getReader();"
    "check((await r.read()).value===3&&(await r.read()).done,'retained source')}"
    "{let cancels=0;const reason={},s=new ReadableStream({pull(c){c.enqueue(1)},"
    "cancel(r){check(r===reason,'write failure reason');cancels++}}),"
    "d=new WritableStream({write(){throw reason}});check(await s.pipeTo(d).catch(e=>e)"
    "===reason&&cancels===1&&!s.locked&&!d.locked,'write failure cleanup')}"
    "{const s=new ReadableStream({start(c){c.close()}}),d=new WritableStream();"
    "await s.pipeTo(d,{preventClose:true});const w=d.getWriter();await w.write(1);"
    "await w.close();w.releaseLock()}"
    "pocSummary='OK'})().catch(e=>pocSummary=String(e));";

static const char readable_buffered_body[] =
    "(async()=>{let c;const s=new ReadableStream({start(v){c=v;"
    "v.enqueue(new Uint8Array([1,2]));v.enqueue(new Uint8Array([3]));v.close()}});"
    "const consume=__tilefinchConsumeBufferedReadableByteStream;"
    "let refused=false;try{consume(s,2)}"
    "catch(e){refused=e instanceof RangeError}if(!refused||s.locked||"
    "__tilefinchReadableStreamDisturbed(s))throw Error('buffer refusal consumed body');"
    "const bytes=consume(s,3);if(!bytes||bytes.join()!=='1,2,3'||"
    "!__tilefinchReadableStreamDisturbed(s)||c.desiredSize!==0)"
    "throw Error('buffered closing body not consumed');"
    "const r=s.getReader();await r.closed;if(!(await r.read()).done)"
    "throw Error('buffered drain not closed');const pending=new ReadableStream();"
    "if(consume(pending)!==null||__tilefinchReadableStreamDisturbed(pending))"
    "throw Error('unfinished body consumed');await pending.cancel();"
    "pocSummary='OK'})().catch(e=>pocSummary=String(e));";
