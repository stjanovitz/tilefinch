static int test_opfs_sync_access(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    budget_install_lexbor(&budget);
    static const char worker_lifecycle_page[] =
        "<!doctype html><body></body>";
    PocDocument worker_performance_document = {0};
    ScriptResult worker_performance_result = {0};
    ScriptRuntime *worker_performance_runtime = NULL;
    BrowserSession storage_manager_session;
    puts("test: dedicated Worker exposes a bounded OPFS sync access handle");
    CHECK(document_parse(
        &worker_performance_document, &budget, worker_lifecycle_page,
        strlen(worker_lifecycle_page), 8));
    CHECK(browser_session_init(&storage_manager_session, &budget, 64 * 1024));
    memset(&worker_performance_result, 0,
           sizeof(worker_performance_result));
    worker_performance_runtime = script_runtime_create_deferred(
        &worker_performance_document, &budget, 5 * MIB, 1000,
        "https://opfs-sync.test/", &storage_manager_session,
        &worker_performance_result);
    CHECK(worker_performance_runtime != NULL
          && script_runtime_evaluate_diagnostic(
              worker_performance_runtime,
              "(()=>{pocSummary='pending';const source=\"(async()=>{const root="
              "await navigator.storage.getDirectory(),file=await root."
              "getFileHandle('sync.bin',{create:true}),handle=await file."
              "createSyncAccessHandle();let invalidPromise,invalidSync=false;try{"
              "invalidPromise=FileSystemFileHandle.prototype.createSyncAccessHandle."
              "call({})}catch(error){invalidSync=true}const invalidRejected=!invalidSync"
              "&&invalidPromise instanceof Promise&&await invalidPromise.then(()=>false,"
              "error=>error&&error.name==='TypeError'),input=new Uint8Array([7,8,9]),options="
              "Object.create({at:1});options.unused=()=>0;let locked="
              "false;try{await file.createWritable()}catch(error){locked=error.name"
              "==='NoModificationAllowedError'}const first=handle.write(input."
              "subarray(0,1)),written=first+handle.write(input.subarray(1)),size="
              "handle.getSize(),output=new Uint8Array(2),read=handle.read(output,"
              "options),wide=new Uint8Array(1024*1024),wideRead=handle.read(wide,"
              "{at:0}),eofRead=handle.read(wide,{at:99});handle.truncate(2);handle."
              "truncate(4);const zeroes=new Uint8Array(2),zeroRead=handle.read(zeroes,"
              "{at:2});handle.truncate(2);handle.flush();handle.close();handle.close();"
              "const unit=new Uint8Array([1]),chunk=new Uint8Array(64),block=new "
              "Uint8Array(4096),oneFile=await root.getFileHandle('one.bin',{create:true}),"
              "one=await oneFile.createSyncAccessHandle();for(let i=0;i<4096;i++)one."
              "write(unit);one.close();const chunkFile=await root.getFileHandle('chunk."
              "bin',{create:true}),chunked=await chunkFile.createSyncAccessHandle();for("
              "let i=0;i<64;i++)chunked.write(chunk);chunked.close();const blockFile=await"
              " root.getFileHandle('block.bin',{create:true}),blocked=await blockFile."
              "createSyncAccessHandle();blocked.write(block);blocked.close();"
              "const aliasFile=await root.getFileHandle('alias.bin',{create:true}),"
              "alias=await aliasFile.createSyncAccessHandle(),backing=new Uint8Array("
              "1024*1024);backing.set([99,21,22,98]);const slice=new DataView(backing."
              "buffer,1,2);alias.write(slice);backing.fill(0);const destination=new "
              "Uint8Array([77,0,0,88]);alias.read(new DataView(destination.buffer,1,2),"
              "{at:0});destination[1]=0;const independent=new Uint8Array(2);alias.read("
              "independent,{at:0});alias.close();const aliasOk=destination.join(',')==="
              "'77,0,22,88'&&independent.join(',')==='21,22';const saved="
              "Uint8Array.prototype.set;Uint8Array.prototype.set=()=>{throw Error('poison')};"
              "const encoded=new TextEncoder().encode('worker-copy');Uint8Array.prototype."
              "set=saved;"
              "postMessage({written,size,read,bytes:Array.from(output),locked,aliasOk,tag:"
              "Object.prototype.toString.call(handle),ctor:handle instanceof "
              "FileSystemSyncAccessHandle,invalidRejected,wideRead,eofRead,wide:Array."
              "from(wide.subarray(0,3)),zeroRead,zeroes:Array.from(zeroes),encoded:Array."
              "from(encoded)})})().catch(error=>postMessage"
              "({error:error.name+':'+error.message}))\",url=URL.createObjectURL(new Blob(["
              "source])),worker=new Worker(url);worker.onmessage=async event=>{try{"
              "const root=await navigator.storage.getDirectory(),file=await root."
              "getFileHandle('sync.bin'),bytes=new Uint8Array(await(await file."
              "getFile()).arrayBuffer()),writer=await file.createWritable();await "
              "writer.close();const data=event.data;pocSummary=!data.error&&data."
              "written===3&&data.size===3&&data.read===2&&data.bytes.join(',')==='8,9'"
              "&&data.wideRead===3&&data.eofRead===0&&data.wide.join(',')==='7,8,9'"
              "&&data.zeroRead===2&&data.zeroes.join(',')==='0,0'&&data.encoded.join(',')"
              "==='119,111,114,107,101,114,45,99,111,112,121'"
              "&&data.locked&&data.aliasOk&&data.ctor&&data.invalidRejected&&data.tag==='[object "
              "FileSystemSyncAccessHandle]'&&bytes.length===2&&bytes[0]===7&&bytes[1]"
              "===8&&__tilefinchRootCensus.opfsBufferGrowths<=17&&"
              "__tilefinchRootCensus.opfsBufferCopiedBytes<=8192&&"
              "__tilefinchRootCensus.workerOpfsReadScratchBytes===0&&"
              "__tilefinchRootCensus.workerOpfsReadCloneBytes===0?"
              "'OPFS-SYNC-OK':'OPFS-SYNC-BAD:'+JSON.stringify({data,bytes:Array."
              "from(bytes)});worker.terminate();URL.revokeObjectURL(url)}catch(error){"
              "pocSummary='OPFS-SYNC-ERROR:'+error.name+':'+error.message}}})()",
              "<opfs-worker-sync-access>", &worker_performance_result));
    for (size_t worker_turn = 0; worker_turn < 64; worker_turn++) {
        bool advanced = script_runtime_advance(
            worker_performance_runtime, worker_turn, 32,
            &worker_performance_result);
        if (!advanced) {
            fprintf(stderr, "OPFS advance turn=%zu interrupted=%d "
                "watchdog-ms=%lu error=%s\n", worker_turn,
                (int) worker_performance_result.interrupted,
                worker_performance_result.watchdog_elapsed_ms,
                worker_performance_result.error);
        }
        CHECK(advanced);
    }
    if (strcmp(worker_performance_result.summary,
               "OPFS-SYNC-OK") != 0) {
        fprintf(stderr, "OPFS sync summary=%s error=%s\n",
                worker_performance_result.summary,
                worker_performance_result.error);
    }
    CHECK(strcmp(worker_performance_result.summary,
                 "OPFS-SYNC-OK") == 0);
    script_runtime_destroy(worker_performance_runtime);
    document_destroy(&worker_performance_document);
    browser_session_destroy(&storage_manager_session);
    CHECK(budget.current == 0);
    return 0;
}
