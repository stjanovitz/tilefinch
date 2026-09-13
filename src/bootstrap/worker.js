/* Native installs this source first, removes the temporary installer property,
   and only then passes the privileged compiler as an argument.  Author code
   can poison ordinary constructors before this lazy module runs, but no such
   constructor can observe a compiler bridge on the global object. */
globalThis.__tilefinchInstallWorker = (
  runWorkerNative,
  traceWorkerNative,
  createWorkerRealmNative,
  destroyWorkerRealmNative,
  creatorOrigin,
  creatorSecureContext,
  creatorURL,
) => {
  /* Worker is intentionally a first-use module. Ordinary pages should not pay
     to compile its message/lifecycle machinery merely because the constructor
     is standards-visible. */
  const blobForURL = globalThis.__tilefinchBlobForURL,
    trustedString = globalThis.__tilefinchTrustedString,
    workerIntrinsics = globalThis.__tilefinchWorkerIntrinsics,
    cloneWorkerValue = globalThis.__tilefinchCloneWorkerValue,
    workerCloneIntrinsics = globalThis.__tilefinchWorkerCloneIntrinsics,
    createPrivateWeakMap = globalThis.__tilefinchCreatePrivateWeakMap,
    createMessageEvent = globalThis.__tilefinchCreateMessageEvent,
    fetchForWorker = globalThis.__tilefinchFetchForWorker,
    fetchWorkerScript = globalThis.__tilefinchFetchWorkerScript,
    fetchWorkerScriptSync = globalThis.__tilefinchFetchWorkerScriptSync,
    PerformanceEntryCtor = globalThis.PerformanceEntry,
    PerformanceResourceTimingCtor = globalThis.PerformanceResourceTiming,
    PerformanceObserverEntryListCtor = globalThis.PerformanceObserverEntryList,
    trustedEvent = globalThis.__tilefinchTrustedEvent,
    markNative = globalThis.__tilefinchMarkNativeFunction,
    invokeEventTargetCheckpointed =
      globalThis.__tilefinchInvokeEventTargetCheckpointed,
    createWorkerCrypto = globalThis.__tilefinchCreateWorkerCrypto,
    setCryptoBufferNormalizer =
      globalThis.__tilefinchSetCryptoBufferNormalizer,
    normalizeTimerDelay = globalThis.__tilefinchNormalizeTimerDelay,
    scheduleTimeout = globalThis.__tilefinchScheduleTimeout,
    scheduleInterval = globalThis.__tilefinchScheduleInterval,
    cancelTimer = globalThis.__tilefinchCancelTimer,
    createWorkerPerformance = globalThis.__tilefinchCreateWorkerPerformance,
    createTrustedTypesRealm =
      globalThis.__tilefinchCreateTrustedTypesRealm,
    abortXHRForWorker = globalThis.__tilefinchAbortXHRForWorker,
    abortFileReaderForWorker =
      globalThis.__tilefinchAbortFileReaderForWorker,
    abortIndexedDBForWorker = (database) => {
      const abort = globalThis.__tilefinchAbortIndexedDBForWorker;
      return typeof abort === "function" ? abort(database) : false;
    },
    blobTextForWorker = globalThis.__tilefinchBlobTextForWorker,
    closeWebSocketForWorker =
      globalThis.__tilefinchCloseWebSocketForWorker,
    ownerNavigator = globalThis.navigator,
    workerOrigin = trustedString(creatorOrigin),
    workerSecureContext = creatorSecureContext === true,
    workerCreatorURL = trustedString(creatorURL);
  if (
    typeof runWorkerNative !== "function" ||
    typeof traceWorkerNative !== "function" ||
    typeof createWorkerRealmNative !== "function" ||
    typeof destroyWorkerRealmNative !== "function" ||
    typeof blobForURL !== "function" ||
    typeof trustedString !== "function" ||
    !workerIntrinsics || typeof workerIntrinsics.sourceForBlob !== "function" ||
    typeof workerIntrinsics.apply !== "function" ||
    typeof cloneWorkerValue !== "function" ||
    !workerCloneIntrinsics ||
    typeof workerCloneIntrinsics.capture !== "function" ||
    !workerCloneIntrinsics.owner ||
    typeof createPrivateWeakMap !== "function" ||
    typeof createMessageEvent !== "function" ||
    typeof fetchForWorker !== "function" ||
    typeof fetchWorkerScript !== "function" ||
    typeof fetchWorkerScriptSync !== "function" ||
    typeof PerformanceEntryCtor !== "function" ||
    typeof PerformanceResourceTimingCtor !== "function" ||
    typeof PerformanceObserverEntryListCtor !== "function" ||
    typeof invokeEventTargetCheckpointed !== "function" ||
    typeof createWorkerCrypto !== "function" ||
    typeof setCryptoBufferNormalizer !== "function" ||
    typeof normalizeTimerDelay !== "function" ||
    typeof scheduleTimeout !== "function" ||
    typeof scheduleInterval !== "function" ||
    typeof cancelTimer !== "function" ||
    typeof createWorkerPerformance !== "function" ||
    typeof createTrustedTypesRealm !== "function" ||
    typeof abortXHRForWorker !== "function" ||
    typeof abortFileReaderForWorker !== "function" ||
    typeof blobTextForWorker !== "function" ||
    typeof closeWebSocketForWorker !== "function" ||
    typeof trustedEvent !== "function"
  )
    throw new Error("Worker bootstrap is unavailable");

  const workerSourceForBlob = (blob) =>
    workerIntrinsics.sourceForBlob(blob);

  /* Dedicated workers share selected Web Platform constructors with their
     owner, but never the owner's arbitrary global properties.  Keep this
     list explicit: the worker realm has its own ECMAScript intrinsics, while
     lazily installed platform groups (WebAssembly, streams, WebSocket, ...)
     remain discoverable after Worker construction. */
  const workerSharedGlobalNames = new Set([
    "AbortController", "AbortSignal", "AnimationEvent", "BroadcastChannel", "ByteLengthQueuingStrategy",
    "CompressionStream", "CountQueuingStrategy", "Crypto", "CryptoKey",
    "DecompressionStream", "DOMException", "DOMRect", "ErrorEvent", "Event", "EventSource",
    "EventTarget", "File", "FileList", "FileReader", "FormData",
    "FileReaderSync",
    "MessageChannel", "MessageEvent", "MessagePort",
    "PerformanceEntry", "PerformanceMeasure", "PerformanceMark",
    "PerformanceObserverEntryList", "PerformanceResourceTiming", "ReadableStream",
    "ReadableStreamBYOBReader", "ReadableStreamBYOBRequest",
    "ReadableStreamDefaultController", "ReadableStreamDefaultReader",
    "CloseEvent", "CustomEvent", "ProgressEvent", "XMLHttpRequest",
    "XMLHttpRequestEventTarget", "XMLHttpRequestUpload",
    "IDBCursor", "IDBCursorWithValue", "IDBDatabase", "IDBFactory", "IDBIndex",
    "IDBKeyRange", "IDBObjectStore", "IDBOpenDBRequest", "IDBRequest",
    "IDBTransaction", "indexedDB", "Intl",
    "GPU", "NavigatorUAData", "NetworkInformation", "Permissions",
    "PermissionStatus", "StorageManager", "SubtleCrypto", "TextDecoderStream",
    "TextEncoderStream", "TransformStream", "WGSLLanguageFeatures",
    "TransformStreamDefaultController", "WebAssembly", "WebSocket", "WritableStream",
    "WritableStreamDefaultController", "WritableStreamDefaultWriter", "console",
  ]);
  /* The owner-backed fallback above keeps platform groups lazy, but returning
     an owner-realm constructor directly gives it the owner's Function
     prototype. Dedicated workers have their own realm, so install worker-local
     lazy accessors for the constructors sites commonly inspect. The accessor
     does not touch the owner group until author code requests the name. */
  const workerRealmConstructorInit = `(()=>{
    const root=globalThis,proto=Object.getPrototypeOf(root),
      names=['AnimationEvent','XMLHttpRequest','XMLHttpRequestEventTarget',
        'XMLHttpRequestUpload','FileReader','File','FileList','CustomEvent',
        'ProgressEvent','CloseEvent','DOMRect','IDBFactory','IDBDatabase',
        'IDBRequest','IDBOpenDBRequest','IDBKeyRange','IDBTransaction',
        'IDBObjectStore','IDBIndex','IDBCursor','IDBCursorWithValue',
        'Permissions','PermissionStatus','StorageManager','GPU',
        'WGSLLanguageFeatures'],
      localMethod=(original,name)=>{
        if(typeof original!=='function')return original;
        const local=function(...args){return Reflect.apply(original,this,args)};
        try{Object.defineProperty(local,'name',{value:name,configurable:true})}
        catch(_){}
        return local;
      },
      localCallable=(original,name)=>{
        if(typeof original!=='function')return original;
        let local;
        if(Object.prototype.hasOwnProperty.call(original,'prototype')){
          local=function(...args){
            if(new.target)return Reflect.construct(original,args,new.target);
            return Reflect.apply(original,this,args);
          };
          const originalPrototype=original.prototype,
            inherited=Object.create(Object.getPrototypeOf(originalPrototype));
          for(const key of Reflect.ownKeys(originalPrototype)){
            if(key==='constructor')continue;
            const descriptor=Object.getOwnPropertyDescriptor(
              originalPrototype,key);
            if(!descriptor)continue;
            if(typeof descriptor.value==='function')
              descriptor.value=localMethod(descriptor.value,String(key));
            if(typeof descriptor.get==='function')
              descriptor.get=localMethod(descriptor.get,'get '+String(key));
            if(typeof descriptor.set==='function')
              descriptor.set=localMethod(descriptor.set,'set '+String(key));
            try{Object.defineProperty(inherited,key,descriptor)}catch(_){}
          }
          local.prototype=name==='StorageManager'?new Proxy(inherited,{
            get(target,key,receiver){return key==='persist'?undefined:
              Reflect.get(target,key,receiver)},
            has(target,key){return key==='persist'?false:Reflect.has(target,key)}
          }):inherited;
          Object.defineProperty(local.prototype,'constructor',{
            value:local,writable:true,configurable:true});
        }else local=localMethod(original,name);
        try{Object.defineProperty(local,'name',{value:name,configurable:true})}
        catch(_){}
        for(const key of Reflect.ownKeys(original)){
          if(key==='name'||key==='length'||key==='prototype')continue;
          try{Object.defineProperty(local,key,
            Object.getOwnPropertyDescriptor(original,key))}catch(_){}
        }
        return local;
      },
      install=(name,transform=localCallable)=>{
        Object.defineProperty(root,name,{configurable:true,enumerable:false,
          get(){
            const original=Reflect.get(proto,name,root),
              local=transform(original,name);
            Object.defineProperty(root,name,{value:local,writable:true,
              configurable:true,enumerable:false});
            return local;
          },
          set(value){Object.defineProperty(root,name,{value,writable:true,
            configurable:true,enumerable:false})}
        });
      };
    for(const name of names)install(name);
    install('Intl',original=>{
      if(!original||typeof original!=='object')return original;
      const local={};
      for(const key of Reflect.ownKeys(original)){
        const descriptor=Object.getOwnPropertyDescriptor(original,key);
        if(descriptor&&'value'in descriptor&&typeof descriptor.value==='function')
          descriptor.value=localCallable(descriptor.value,String(key));
        try{Object.defineProperty(local,key,descriptor)}catch(_){}
      }
      return local;
    });
    {
      const Base=root.XMLHttpRequest,
        register=root.__tilefinchRegisterWorkerXHR,
        workerURL=root.__tilefinchWorkerScriptURL;
      let at=Base.prototype,responseType;
      while(at&&!responseType){
        responseType=Object.getOwnPropertyDescriptor(at,'responseType');
        at=Object.getPrototypeOf(at);
      }
      function WorkerXMLHttpRequest(...args){
        if(!new.target)throw new TypeError('constructor requires new');
        const xhr=Reflect.construct(Base,args,new.target);
        register(xhr);
        return xhr;
      }
      WorkerXMLHttpRequest.prototype=Object.create(Base.prototype);
      Object.defineProperty(WorkerXMLHttpRequest.prototype,'constructor',{
        configurable:true,writable:true,value:WorkerXMLHttpRequest});
      Object.defineProperties(WorkerXMLHttpRequest.prototype,{
        open:{configurable:true,writable:true,value:function(method,url,
            async=true){
          const text=String(url),
            absolute=/^[A-Za-z][A-Za-z0-9+.-]*:/.test(text);
          if(workerURL.startsWith('blob:')&&!absolute)
            throw new DOMException('Invalid URL','SyntaxError');
          return Base.prototype.open.call(this,method,
            absolute?new URL(text).href:new URL(text,workerURL).href,async);
        }},
        responseType:{configurable:true,
          get(){return responseType.get.call(this)},
          set(value){
            value=String(value);
            if(value!=='document')responseType.set.call(this,value);
          }}
      });
      Object.defineProperty(WorkerXMLHttpRequest,'name',{
        configurable:true,value:'XMLHttpRequest'});
      root.XMLHttpRequest=WorkerXMLHttpRequest;
    }
    {
      const Base=root.FileReader,
        register=root.__tilefinchRegisterWorkerFileReader;
      function WorkerFileReader(...args){
        if(!new.target)throw new TypeError('constructor requires new');
        const reader=Reflect.construct(Base,args,new.target);
        register(reader);
        return reader;
      }
      WorkerFileReader.prototype=Object.create(Base.prototype);
      Object.defineProperty(WorkerFileReader.prototype,'constructor',{
        configurable:true,writable:true,value:WorkerFileReader});
      Object.defineProperty(WorkerFileReader,'name',{
        configurable:true,value:'FileReader'});
      root.FileReader=WorkerFileReader;
    }
    {
      const bytesForBlob=root.__tilefinchWorkerBlobBytes;
      const textForBlob=root.__tilefinchWorkerBlobText;
      class FileReaderSync{
        readAsArrayBuffer(blob){
          const source=bytesForBlob(blob),copy=new Uint8Array(source.length);
          copy.set(source);
          return copy.buffer;
        }
        readAsText(blob,encoding='utf-8'){
          return textForBlob(blob,encoding);
        }
        readAsDataURL(blob){
          const bytes=new Uint8Array(this.readAsArrayBuffer(blob));
          let binary='';
          for(let at=0;at<bytes.length;at+=4096)
            binary+=String.fromCharCode(...bytes.slice(at,at+4096));
          return 'data:'+(blob.type||'application/octet-stream')+
            ';base64,'+btoa(binary);
        }
      }
      root.FileReaderSync=FileReaderSync;
      delete root.__tilefinchWorkerBlobBytes;
      delete root.__tilefinchWorkerBlobText;
    }
    {
      const Base=root.WebSocket,
        register=root.__tilefinchRegisterWorkerWebSocket;
      function WorkerWebSocket(...args){
        if(!new.target)throw new TypeError('constructor requires new');
        const socket=Reflect.construct(Base,args,new.target);
        register(socket);
        return socket;
      }
      WorkerWebSocket.prototype=Object.create(Base.prototype);
      Object.defineProperty(WorkerWebSocket.prototype,'constructor',{
        configurable:true,writable:true,value:WorkerWebSocket});
      for(const key of ['CONNECTING','OPEN','CLOSING','CLOSED'])
        Object.defineProperty(WorkerWebSocket,key,{value:Base[key]});
      Object.defineProperty(WorkerWebSocket,'name',{
        configurable:true,value:'WebSocket'});
      root.WebSocket=WorkerWebSocket;
    }
    {
      const createNested=root.__tilefinchCreateNestedWorker,
        BasePrototype=root.__tilefinchOwnerWorkerPrototype;
      function Worker(url,options){
        if(!new.target)throw new TypeError('constructor requires new');
        const child=createNested(url,options);
        Object.setPrototypeOf(child,new.target.prototype);
        return child;
      }
      Worker.prototype=Object.create(EventTarget.prototype);
      for(const key of Reflect.ownKeys(BasePrototype)){
        if(key==='constructor')continue;
        const descriptor=Object.getOwnPropertyDescriptor(BasePrototype,key);
        try{Object.defineProperty(Worker.prototype,key,descriptor)}catch(_){}
      }
      Object.defineProperty(Worker.prototype,'constructor',{
        configurable:true,writable:true,value:Worker});
      root.Worker=Worker;
      delete root.__tilefinchCreateNestedWorker;
      delete root.__tilefinchOwnerWorkerPrototype;
    }
    {
      const ownerFactoryForWorker=root.__tilefinchGetOwnerIndexedDB,
        register=root.__tilefinchRegisterWorkerIDBRequest;
      delete root.__tilefinchGetOwnerIndexedDB;
      delete root.__tilefinchRegisterWorkerIDBRequest;
      Object.defineProperty(root,'indexedDB',{configurable:true,enumerable:false,
        get(){
          const ownerFactory=ownerFactoryForWorker(),
            LocalFactory=root.IDBFactory,
            localFactory=Object.create(LocalFactory.prototype);
          for(const name of ['open','deleteDatabase'])
            Object.defineProperty(localFactory,name,{configurable:true,
              writable:true,value:function(...args){
                const request=Reflect.apply(
                  ownerFactory[name],ownerFactory,args);
                register(request,name==='open');
                return request;
              }});
          for(const name of ['cmp','databases'])
            Object.defineProperty(localFactory,name,{configurable:true,
              writable:true,value:function(...args){
                return Reflect.apply(ownerFactory[name],ownerFactory,args);
              }});
          Object.defineProperty(root,'indexedDB',{configurable:true,
            enumerable:false,writable:true,value:localFactory});
          return localFactory;
        },
        set(value){Object.defineProperty(root,'indexedDB',{configurable:true,
          enumerable:false,writable:true,value})}
      });
    }
    {
      const importScriptsOwner=root.__tilefinchImportScripts,
        moduleWorker=root.__tilefinchWorkerType==='module';
      root.importScripts=function(...urls){
        if(moduleWorker)
          throw new TypeError('importScripts is unavailable in a module Worker');
        return Reflect.apply(importScriptsOwner,root,urls);
      };
      delete root.__tilefinchImportScripts;
      delete root.__tilefinchWorkerType;
    }
    {
      class PromiseRejectionEvent extends Event{
        constructor(type,init={}){
          super(type,{cancelable:true});
          Object.defineProperties(this,{
            promise:{enumerable:true,value:init.promise},
            reason:{enumerable:true,value:init.reason}
          });
        }
      }
      const pending=new Map();
      Object.defineProperty(root,'PromiseRejectionEvent',{
        configurable:true,writable:true,value:PromiseRejectionEvent});
      Object.defineProperty(root,'__tilefinchQueuePromiseRejection',{
        configurable:false,enumerable:false,writable:false,
        value(promise,reason,handled){
          let record=pending.get(promise);
          if(handled){
            if(!record)return;
            pending.delete(promise);
            if(record.reported)
              root.setTimeout(()=>root.dispatchEvent(
                new PromiseRejectionEvent('rejectionhandled',{promise,reason})),0);
            else root.clearTimeout(record.timer);
            return;
          }
          if(record||pending.size>=16)return;
          record={reported:false,timer:0};
          pending.set(promise,record);
          record.timer=root.setTimeout(()=>{
            if(pending.get(promise)!==record)return;
            record.reported=true;
            root.dispatchEvent(new PromiseRejectionEvent(
              'unhandledrejection',{promise,reason}));
          },0);
          if(!record.timer)pending.delete(promise);
        }
      });
    }
  })()`;

  const traceMissingWorkerCapabilities =
      traceWorkerNative("worker-capability-missing") === true,
    traceWorkerLifecycle =
      traceWorkerNative("worker-lifecycle") === true,
    missingWorkerCapabilities = new Set(),
    traceMissingWorkerCapability = (operation, key) => {
      if (!traceMissingWorkerCapabilities || typeof key !== "string") return;
      const identity = operation + ":" + key;
      if (missingWorkerCapabilities.has(identity)
          || missingWorkerCapabilities.size >= 64) return;
      missingWorkerCapabilities.add(identity);
      traceWorkerNative("worker-capability-missing", {
        operation,
        name: key.slice(0, 128),
      });
    },
    traceWorkerMessage = (direction, value) => {
      traceWorkerNative(direction, value);
    },
    workerStates = createPrivateWeakMap(),
    newHandlerSlots = () => ({
      message: { value: null, wrapper: null },
      error: { value: null, wrapper: null },
      messageerror: { value: null, wrapper: null },
      languagechange: { value: null, wrapper: null },
      offline: { value: null, wrapper: null },
      online: { value: null, wrapper: null },
      rejectionhandled: { value: null, wrapper: null },
      unhandledrejection: { value: null, wrapper: null },
    }),
    setEventHandler = (target, slots, type, value, globalError = false) => {
      const slot = slots[type],
        callback = typeof value === "function" ? value : null;
      if (callback === slot.value) return;
      slot.value = callback;
      if (callback !== null && slot.wrapper === null) {
        /* Event-handler attributes occupy one stable position in the event
           listener list. Replacing a non-null handler preserves that position;
           clearing and later restoring it registers a new position. */
        slot.wrapper = function (event) {
          const current = slot.value;
          if (typeof current !== "function") return;
          if (!globalError)
            return workerIntrinsics.apply(current, target, [event]);
          const handled = workerIntrinsics.apply(current, target, [
            event.message,
            event.filename,
            event.lineno,
            event.colno,
            event.error,
          ]);
          /* WorkerGlobalScope.onerror uses the legacy five-argument handler;
             returning true suppresses propagation to the owning Worker. */
          if (handled === true) event.preventDefault();
          return handled;
        };
        EventTarget.prototype.addEventListener.call(
          target, type, slot.wrapper, false);
      } else if (callback === null && slot.wrapper !== null) {
        EventTarget.prototype.removeEventListener.call(
          target, type, slot.wrapper, false);
        slot.wrapper = null;
      }
    },
    workerValueShape = (value) => {
      if (value === null) return { type: "null" };
      const type = typeof value;
      if (type !== "object") return { type };
      const tag = Object.prototype.toString.call(value),
        keys = Reflect.ownKeys(value).filter(key => typeof key === "string")
          .slice(0, 32),
        fields = [];
      for (const key of keys) {
        const descriptor = Object.getOwnPropertyDescriptor(value, key);
        fields.push([
          key.slice(0, 64),
          descriptor && "value" in descriptor
            ? descriptor.value === null ? "null" : typeof descriptor.value
            : "accessor",
        ]);
      }
      return { type, tag, keys: fields, truncated: keys.length >= 32 };
    },
    reportOwnerListenerError = (state, type, error, item, list) => {
      const handler = state.handlers[type]?.wrapper;
      traceWorkerMessage("owner-listener-error", {
        type: String(type),
        handler: item?.callback === handler,
        ordinal: list.indexOf(item),
        listeners: list.length,
        once: !!item?.once,
        active: !!item?.active,
        signal: !!item?.signal,
        message: String((error && error.message) || error),
        stack: workerIntrinsics.slice(
          trustedString((error && error.stack) || ""), 0, 4096),
      });
      /* Safe companion trace: property names and types only, never challenge
         values or signed payloads. This can remain enabled while diagnosing
         a live worker without retaining its telemetry. */
      traceWorkerMessage("owner-listener-error-shape", {
        message: String((error && error.message) || error),
        eventType: String(type),
        handler: item?.callback === handler,
        ordinal: list.indexOf(item),
        listeners: list.length,
        once: !!item?.once,
        capture: !!item?.capture,
        callbackType: typeof item?.callback,
        shape: workerValueShape(state.dispatchMessageData),
        targetIsWorker: state.dispatchEvent?.target === state.dispatchOwner,
        currentTargetIsWorker:
          state.dispatchEvent?.currentTarget === state.dispatchOwner,
        active: !!state.active,
        started: state.startTimer === 0,
        closing: !!state.closing,
        realm: !!state.realm,
      });
    },
    emitWorker = (owner, type, event = {}, complete = null) => {
      const state = workerStates.get(owner);
      if (!state || !state.active) return;
      if (!(event instanceof Event)) event = new Event(type);
      if (type === "message")
        traceWorkerMessage("deliver-to-owner", event.data);
      globalThis.__tilefinchPrepareEvent(event, owner, [owner]);
      event.currentTarget = owner;
      event.eventPhase = Event.AT_TARGET;
      state.dispatchType = type;
      state.dispatchMessageData = type === "message" ? event.data : undefined;
      state.dispatchOwner = owner;
      state.dispatchEvent = event;
      let eventFinished = false;
      const finish = () => {
        if (eventFinished) return;
        eventFinished = true;
        state.dispatchMessageData = undefined;
        state.dispatchOwner = null;
        state.dispatchEvent = null;
        globalThis.__tilefinchFinishEventDispatch(event);
        globalThis.__tilefinchRecordEvent();
        if (typeof complete === "function") complete();
      };
      if (type === "message") {
        try {
          if (!event.__stopped)
            invokeEventTargetCheckpointed(
              owner,
              event,
              Event.AT_TARGET,
              state.errorObserver,
              null,
              finish,
            );
          else finish();
        } catch (error) {
          finish();
          throw error;
        }
        return true;
      }
      try {
        if (!event.__stopped) {
          globalThis.__tilefinchInvokeEventTarget(
            owner, event, true, Event.AT_TARGET, state.errorObserver);
          if (!event.__immediateStopped)
            globalThis.__tilefinchInvokeEventTarget(
              owner, event, false, Event.AT_TARGET, state.errorObserver);
        }
      } finally {
        finish();
      }
      return !event.defaultPrevented;
    },
    workerErrorEvent = (error, url, exposeError = false) =>
      trustedEvent(new ErrorEvent("error", {
        cancelable: true,
        message: String((error && error.message) || error),
        filename: String(url || ""),
        lineno: 0,
        colno: 0,
        /* The creator does not receive a cross-realm exception object. */
        error: exposeError ? error : null,
      })),
    dispatchWorkerGlobalError = (state, error, url) => {
      if (!state || !state.active || !state.scope) return false;
      const event = workerErrorEvent(error, url, true),
        target = state.scope;
      globalThis.__tilefinchPrepareEvent(event, target, [target]);
      try {
        if (!event.__stopped) {
          globalThis.__tilefinchInvokeEventTarget(
            target, event, true, Event.AT_TARGET);
          if (!event.__immediateStopped)
            globalThis.__tilefinchInvokeEventTarget(
              target, event, false, Event.AT_TARGET);
        }
      } finally {
        globalThis.__tilefinchFinishEventDispatch(event);
        globalThis.__tilefinchRecordEvent();
      }
      return event.defaultPrevented;
    },
    reportWorkerError = (owner, error, url) => {
      /* The owner's error listener may itself throw and replace the useful
         diagnostic.  Reuse the validation-only native trace seam to retain
         the originating Worker exception without exposing it to page code. */
      traceWorkerMessage("worker-error", {
        message: String((error && error.message) || error),
        stack: String((error && error.stack) || "").slice(0, 4096),
      });
      const state = workerStates.get(owner);
      if (dispatchWorkerGlobalError(state, error, url)) return;
      if (emitWorker(owner, "error", workerErrorEvent(error, url)))
        globalThis.__tilefinchReportUncaught(error, "worker");
    },
    workerPerformanceStates = createPrivateWeakMap(),
    workerPerformanceEntryTypes = Object.freeze([
      "mark", "measure", "resource",
    ]),
    workerPerformanceObserverStates = createPrivateWeakMap(),
    workerPerformanceEntryLimit = 128,
    workerPerformanceObserverLimit = 16,
    workerPerformanceObserverRecordLimit = 64,
    workerPerformanceMarkTime = (state, name) => {
      name = String(name);
      for (let at = state.entries.length - 1; at >= 0; at--) {
        const entry = state.entries[at];
        if (entry.entryType === "mark" && entry.name === name)
          return entry.startTime;
      }
      throw new DOMException("The mark " + name + " does not exist", "SyntaxError");
    },
    sortWorkerPerformanceEntries = entries => entries.slice().sort(
      (left, right) => left.startTime - right.startTime),
    appendWorkerPerformanceEntry = (performance, entry) => {
      const state = workerPerformanceStates.get(performance);
      if (!state) throw new TypeError("Illegal invocation");
      if (state.entries.length >= workerPerformanceEntryLimit)
        state.entries.shift();
      state.entries.push(entry);
      for (const observer of state.observers) {
        const observerState = workerPerformanceObserverStates.get(observer);
        if (!observerState || !observerState.types.has(entry.entryType)) continue;
        if (observerState.records.length >= workerPerformanceObserverRecordLimit) {
          observerState.records.shift();
          observerState.dropped++;
        }
        observerState.records.push(entry);
        observerState.schedule();
      }
      return entry;
    },
    WorkerPerformance = class Performance extends EventTarget {
      constructor() { super(); throw new TypeError("Illegal constructor"); }
      get timeOrigin() {
        const state = workerPerformanceStates.get(this);
        if (!state) throw new TypeError("Illegal invocation");
        return state.timeOrigin;
      }
      now() {
        const state = workerPerformanceStates.get(this);
        if (!state) throw new TypeError("Illegal invocation");
        return state.clock.now();
      }
      mark(name, options = {}) {
        const state = workerPerformanceStates.get(this);
        if (!state) throw new TypeError("Illegal invocation");
        const start = options.startTime === undefined
          ? this.now() : Number(options.startTime);
        if (!Number.isFinite(start) || start < 0)
          throw new TypeError("startTime must be a finite nonnegative number");
        const entry = new PerformanceEntryCtor(String(name), "mark", start, 0);
        entry.detail = options.detail;
        return appendWorkerPerformanceEntry(this, entry);
      }
      measure(name, startOrOptions, endMark) {
        const state = workerPerformanceStates.get(this);
        if (!state) throw new TypeError("Illegal invocation");
        const timestamp = value => typeof value === "string"
          ? workerPerformanceMarkTime(state, value) : Number(value);
        let start = 0, end, detail;
        if (typeof startOrOptions === "object" && startOrOptions !== null) {
          const hasStart = startOrOptions.start !== undefined,
            hasEnd = startOrOptions.end !== undefined,
            hasDuration = startOrOptions.duration !== undefined;
          if ((hasStart && hasEnd && hasDuration) || (!hasStart && !hasEnd))
            throw new TypeError("Invalid measure options");
          start = hasStart ? timestamp(startOrOptions.start) : NaN;
          end = hasEnd ? timestamp(startOrOptions.end) : NaN;
          const duration = hasDuration ? Number(startOrOptions.duration) : NaN;
          if (hasDuration && !Number.isFinite(duration))
            throw new TypeError("duration must be finite");
          if (!hasStart) start = end - duration;
          else if (!hasEnd) end = hasDuration ? start + duration : this.now();
          detail = startOrOptions.detail;
        } else {
          if (startOrOptions !== undefined) start = timestamp(startOrOptions);
          end = endMark !== undefined ? timestamp(endMark) : this.now();
        }
        if (!Number.isFinite(start) || !Number.isFinite(end))
          throw new TypeError("measure timestamps must be finite");
        const entry = new PerformanceEntryCtor(
          String(name), "measure", start, Math.max(0, end - start));
        entry.detail = detail;
        return appendWorkerPerformanceEntry(this, entry);
      }
      getEntries() {
        const state = workerPerformanceStates.get(this);
        if (!state) throw new TypeError("Illegal invocation");
        return sortWorkerPerformanceEntries(state.entries);
      }
      getEntriesByType(type) {
        const state = workerPerformanceStates.get(this);
        if (!state) throw new TypeError("Illegal invocation");
        type = String(type);
        return sortWorkerPerformanceEntries(
          state.entries.filter(entry => entry.entryType === type));
      }
      getEntriesByName(name, type) {
        const state = workerPerformanceStates.get(this);
        if (!state) throw new TypeError("Illegal invocation");
        name = String(name);
        if (type !== undefined) type = String(type);
        return sortWorkerPerformanceEntries(state.entries.filter(
          entry => entry.name === name &&
            (type === undefined || entry.entryType === type)));
      }
      clearMarks(name) {
        const state = workerPerformanceStates.get(this);
        if (!state) throw new TypeError("Illegal invocation");
        if (name !== undefined) name = String(name);
        for (let at = state.entries.length - 1; at >= 0; at--)
          if (state.entries[at].entryType === "mark" &&
              (name === undefined || state.entries[at].name === name))
            state.entries.splice(at, 1);
      }
      clearMeasures(name) {
        const state = workerPerformanceStates.get(this);
        if (!state) throw new TypeError("Illegal invocation");
        if (name !== undefined) name = String(name);
        for (let at = state.entries.length - 1; at >= 0; at--)
          if (state.entries[at].entryType === "measure" &&
              (name === undefined || state.entries[at].name === name))
            state.entries.splice(at, 1);
      }
      clearResourceTimings() {
        const state = workerPerformanceStates.get(this);
        if (!state) throw new TypeError("Illegal invocation");
        for (let at = state.entries.length - 1; at >= 0; at--)
          if (state.entries[at].entryType === "resource")
            state.entries.splice(at, 1);
      }
      setResourceTimingBufferSize() {}
      toJSON() { return { timeOrigin: this.timeOrigin }; }
    },
    createDedicatedWorkerPerformance = () => {
      const value = Object.create(WorkerPerformance.prototype),
        clock = createWorkerPerformance();
      workerPerformanceStates.set(value, {
        timeOrigin: clock.timeOrigin,
        clock,
        entries: [],
        observers: new Set(),
      });
      return value;
    };
  Object.defineProperty(WorkerPerformance.prototype, Symbol.toStringTag, {
    configurable: true,
    value: "Performance",
  });
  markNative(WorkerPerformance);
  for (const key of Reflect.ownKeys(WorkerPerformance.prototype)) {
    const descriptor = Object.getOwnPropertyDescriptor(
      WorkerPerformance.prototype, key,
    );
    markNative(descriptor?.value);
    markNative(descriptor?.get);
    markNative(descriptor?.set);
  }
  let activeWorkers = 0, nextWorkerId = 1;
  const workerProgressClock = createWorkerPerformance(),
    workerProgress = {
      starts: 0, startCompletions: 0, startFailures: 0,
      constructorOptionArguments: 0,
      timerCallbacks: 0, pendingTimers: 0,
      inboundQueued: 0, inboundDelivered: 0, inboundDropped: 0,
      outboundQueued: 0, outboundDelivered: 0, outboundDropped: 0,
      outboundDroppedInactive: 0, outboundDroppedQueue: 0,
      inboundTransferArguments: 0, outboundTransferArguments: 0,
      terminations: 0, lastId: 0, lastTerminationReason: 0,
      lastTaskMs: 0,
    },
    workerProgressProperties = {
      workers: () => activeWorkers,
      workerStarts: () => workerProgress.starts,
      workerStartCompletions: () => workerProgress.startCompletions,
      workerStartFailures: () => workerProgress.startFailures,
      workerConstructorOptionArguments: () =>
        workerProgress.constructorOptionArguments,
      workerTimerCallbacks: () => workerProgress.timerCallbacks,
      workerPendingTimers: () => workerProgress.pendingTimers,
      workerInboundQueued: () => workerProgress.inboundQueued,
      workerInboundDelivered: () => workerProgress.inboundDelivered,
      workerInboundDropped: () => workerProgress.inboundDropped,
      workerOutboundQueued: () => workerProgress.outboundQueued,
      workerOutboundDelivered: () => workerProgress.outboundDelivered,
      workerOutboundDropped: () => workerProgress.outboundDropped,
      workerOutboundDroppedInactive: () =>
        workerProgress.outboundDroppedInactive,
      workerOutboundDroppedQueue: () => workerProgress.outboundDroppedQueue,
      workerInboundTransferArguments: () =>
        workerProgress.inboundTransferArguments,
      workerOutboundTransferArguments: () =>
        workerProgress.outboundTransferArguments,
      workerTerminations: () => workerProgress.terminations,
      workerLastId: () => workerProgress.lastId,
      workerLastTerminationReason: () => workerProgress.lastTerminationReason,
      workerLastTaskMs: () => workerProgress.lastTaskMs,
    };
  for (const key of Object.keys(workerProgressProperties)) {
    Object.defineProperty(globalThis.__tilefinchRootCensus, key, {
      get: workerProgressProperties[key],
    });
  }
  globalThis.Worker = class Worker extends EventTarget {
    constructor(url, options) {
      super();
      if (arguments.length > 1) workerProgress.constructorOptionArguments++;
      /* WebIDL string conversion is observable author code.  Snapshot it once
         and bind Blob lookup, CSP admission, location and diagnostics to that
         exact value so a stateful toString() cannot split validation from
         use. */
      const workerReference = trustedString(url),
        workerURL = new URL(workerReference, workerCreatorURL).href,
        workerOptions = options == null ? null : options,
        workerName = workerOptions == null || workerOptions.name === undefined
          ? "" : trustedString(workerOptions.name),
        workerType = workerOptions == null || workerOptions.type === undefined
          ? "classic" : trustedString(workerOptions.type),
        workerCredentials =
          workerOptions == null || workerOptions.credentials === undefined
            ? "same-origin" : trustedString(workerOptions.credentials),
        blob = blobForURL(workerURL),
        networkWorker = !blob && /^https?:/.test(workerURL);
      if (workerType !== "classic" && workerType !== "module")
        throw new TypeError("unsupported Worker type");
      if (!["omit", "same-origin", "include"].includes(workerCredentials))
        throw new TypeError("unsupported Worker credentials mode");
      if (!blob && !networkWorker)
        throw new DOMException("Invalid Worker URL", "SyntaxError");
      if (activeWorkers >= 2) throw new RangeError("worker quota exceeded");
      activeWorkers++;
      workerProgress.starts++;
      let state = null;
      try {
      const owner = this,
        workerLocationURL = new URL(workerURL),
        WorkerGlobalScope = class WorkerGlobalScope extends EventTarget {
          constructor() {
            super();
            throw new TypeError("Illegal constructor");
          }
        },
        DedicatedWorkerGlobalScope = class DedicatedWorkerGlobalScope
          extends WorkerGlobalScope {
          constructor() {
            super();
          }
        },
        WorkerNavigator = class WorkerNavigator {
          constructor() { throw new TypeError("Illegal constructor"); }
          get userAgent() { return String(ownerNavigator.userAgent || ""); }
          get language() { return String(ownerNavigator.language || ""); }
          get languages() { return workerLanguages; }
          get platform() { return String(ownerNavigator.platform || ""); }
          get onLine() { return !!ownerNavigator.onLine; }
          get productSub() { return String(ownerNavigator.productSub || ""); }
          get vendor() { return String(ownerNavigator.vendor || ""); }
          get vendorSub() { return String(ownerNavigator.vendorSub || ""); }
          get hardwareConcurrency() {
            return Math.max(1, Number(ownerNavigator.hardwareConcurrency) || 1);
          }
          get deviceMemory() {
            return Number(ownerNavigator.deviceMemory) || 0.25;
          }
          get userAgentData() { return ownerNavigator.userAgentData; }
          get connection() { return ownerNavigator.connection; }
          get permissions() { return ownerNavigator.permissions; }
          get storage() { return ownerNavigator.storage; }
          get gpu() { return ownerNavigator.gpu; }
        },
        WorkerLocation = class WorkerLocation {
          constructor() { throw new TypeError("Illegal constructor"); }
        },
        scope = Object.create(DedicatedWorkerGlobalScope.prototype);
      let workerNavigator = null, workerLocation = null,
        workerUAData = null, workerPermissions = null,
        workerStorage = null, workerGPU = null;
      Object.defineProperties(WorkerGlobalScope.prototype, {
        self: {
          configurable: true,
          enumerable: true,
          get() {
            if (this !== state.scope && this !== scope)
              throw new TypeError("Illegal invocation");
            return this;
          },
        },
        origin: {
          configurable: true,
          enumerable: true,
          get() { return workerOrigin; },
        },
        isSecureContext: {
          configurable: true,
          enumerable: true,
          get() { return workerSecureContext; },
        },
      });
        state = {
          id: nextWorkerId++,
          active: true,
          startTimer: 0,
          handlers: newHandlerSlots(),
          scopeHandlers: newHandlerSlots(),
          timers: new Set(),
          timerNestingById: new Map(),
          currentTimerNesting: -1,
          xhrs: new Set(),
          fileReaders: new Set(),
          webSockets: new Set(),
          idbConnections: new Set(),
          children: new Set(),
          scope: null,
          dispatchType: "",
          dispatchOwner: null,
          dispatchEvent: null,
          errorObserver: null,
      };
      state.errorObserver = (error, item, list) =>
        reportOwnerListenerError(
          state, state.dispatchType, error, item, list);
      Object.defineProperty(WorkerGlobalScope.prototype, Symbol.toStringTag, {
        configurable: true,
        value: "WorkerGlobalScope",
      });
      Object.defineProperty(
        DedicatedWorkerGlobalScope.prototype,
        Symbol.toStringTag,
        { configurable: true, value: "DedicatedWorkerGlobalScope" },
      );
      Object.defineProperty(DedicatedWorkerGlobalScope.prototype, "name", {
        configurable: true,
        enumerable: true,
        get() {
          if (this !== state.scope && this !== scope)
            throw new TypeError("Illegal invocation");
          return workerName;
        },
        /* HTML marks this readonly attribute [Replaceable]. Assignment
           shadows the getter only on this worker-global object. */
        set(value) {
          if (this !== state.scope && this !== scope)
            throw new TypeError("Illegal invocation");
          Object.defineProperty(this, "name", {
            configurable: true,
            enumerable: true,
            writable: true,
            value,
          });
        },
      });
      Object.defineProperty(WorkerNavigator.prototype, Symbol.toStringTag, {
        configurable: true,
        value: "WorkerNavigator",
      });
      Object.defineProperty(WorkerLocation.prototype, Symbol.toStringTag, {
        configurable: true,
        value: "WorkerLocation",
      });
      for (const constructor of [
        WorkerGlobalScope,
        DedicatedWorkerGlobalScope,
        WorkerNavigator,
        WorkerLocation,
      ]) {
        markNative(constructor);
        for (const key of Reflect.ownKeys(constructor.prototype)) {
          const descriptor = Object.getOwnPropertyDescriptor(
            constructor.prototype,
            key,
          );
          markNative(descriptor?.value);
          markNative(descriptor?.get);
          markNative(descriptor?.set);
        }
      }
      workerStates.set(this, state);
      scope.WorkerGlobalScope = WorkerGlobalScope;
      scope.DedicatedWorkerGlobalScope = DedicatedWorkerGlobalScope;
      scope.WorkerNavigator = WorkerNavigator;
      scope.WorkerLocation = WorkerLocation;
      scope.postMessage = function (value) {
        if (arguments.length > 1) workerProgress.outboundTransferArguments++;
        traceWorkerMessage("worker-to-owner", value);
        const transferOrOptions = arguments[1],
          transfer = transferOrOptions &&
              typeof transferOrOptions === "object" &&
              !Array.isArray(transferOrOptions) &&
              "transfer" in transferOrOptions
            ? transferOrOptions.transfer : transferOrOptions,
          ports = [],
          copied = cloneWorkerValue(
            value, workerCloneIntrinsics.owner, transfer, ports);
        workerProgress.outboundQueued++;
        workerProgress.lastId = state.id;
        state.outboundPending = (state.outboundPending || 0) + 1;
        const queued = scheduleTimeout(() => {
          let deliveryFinished = false;
          const finishDelivery = (delivered) => {
            if (deliveryFinished) return;
            deliveryFinished = true;
            if (delivered) workerProgress.outboundDelivered++;
            else {
              workerProgress.outboundDropped++;
              workerProgress.outboundDroppedInactive++;
            }
            workerProgress.lastId = state.id;
            workerProgress.lastTaskMs = workerProgressClock.now();
            state.outboundPending--;
            finishWorkerClose(state);
          };
          try {
            const delivered = emitWorker(
              owner,
              "message",
              trustedEvent(createMessageEvent(
                "message", copied, "", null, ports)),
              () => finishDelivery(true),
            ) !== undefined;
            if (!delivered) finishDelivery(false);
          } catch (error) {
            finishDelivery(false);
            throw error;
          }
        }, 0);
        if (!queued) {
          state.outboundPending--;
          workerProgress.outboundDropped++;
          workerProgress.outboundDroppedQueue++;
          finishWorkerClose(state);
        }
      };
      scope.addEventListener = function (type, callback, options = false) {
        return EventTarget.prototype.addEventListener.call(
          this, type, callback, options);
      };
      scope.removeEventListener = function (type, callback, options = false) {
        return EventTarget.prototype.removeEventListener.call(
          this, type, callback, options);
      };
      Object.defineProperties(scope, {
        onmessage: {
          configurable: true,
          get() { return state.scopeHandlers.message.value; },
          set(value) {
            setEventHandler(
              state.scope || scope, state.scopeHandlers, "message", value);
          },
        },
        onmessageerror: {
          configurable: true,
          get() { return state.scopeHandlers.messageerror.value; },
          set(value) {
            setEventHandler(
              state.scope || scope, state.scopeHandlers,
              "messageerror", value);
          },
        },
        onerror: {
          configurable: true,
          get() { return state.scopeHandlers.error.value; },
          set(value) {
            setEventHandler(
              state.scope || scope, state.scopeHandlers, "error", value, true);
          },
        },
      });
      for (const type of [
        "languagechange", "offline", "online",
        "rejectionhandled", "unhandledrejection",
      ]) {
        Object.defineProperty(scope, "on" + type, {
          configurable: true,
          enumerable: true,
          get() { return state.scopeHandlers[type].value; },
          set(value) {
            setEventHandler(
              state.scope || scope, state.scopeHandlers, type, value);
          },
        });
      }
      scope.crypto = createWorkerCrypto();
      scope.performance = createDedicatedWorkerPerformance();
      scope.Performance = WorkerPerformance;
      /* Navigation and paint entries belong to Window, not a dedicated
         worker. Keep both the entry buffer and observers local to this
         DedicatedWorkerGlobalScope rather than aliasing the Window timeline. */
      scope.PerformanceObserver = class PerformanceObserver {
        constructor(callback) {
          if (typeof callback !== "function")
            throw new TypeError("callback required");
          const observer = this,
            observerState = {
              callback,
              types: new Set(),
              records: [],
              pending: false,
              dropped: 0,
              schedule() {
                if (observerState.pending || !observerState.records.length)
                  return;
                observerState.pending = true;
                scope.setTimeout(() => {
                  observerState.pending = false;
                  if (!state.active || state.closing ||
                      !observerState.records.length) return;
                  const records = observerState.records.splice(0),
                    dropped = observerState.dropped;
                  observerState.dropped = 0;
                  try {
                    observerState.callback(
                      new PerformanceObserverEntryListCtor(records),
                      observer,
                      { droppedEntriesCount: dropped },
                    );
                  } catch (error) {
                    reportWorkerError(owner, error, workerURL);
                  }
                }, 0);
              },
            };
          workerPerformanceObserverStates.set(this, observerState);
        }
        observe(options = {}) {
          const observerState = workerPerformanceObserverStates.get(this),
            performanceState = workerPerformanceStates.get(scope.performance);
          if (!observerState || !performanceState)
            throw new TypeError("Illegal invocation");
          options = Object(options);
          const hasEntryTypes = options.entryTypes !== undefined,
            hasType = options.type !== undefined;
          if (hasEntryTypes === hasType)
            throw new TypeError("Specify entryTypes or type");
          const requested = hasType
              ? [String(options.type)]
              : Array.from(options.entryTypes, String),
            types = new Set(requested.filter(type =>
              workerPerformanceEntryTypes.includes(type)));
          if (!performanceState.observers.has(this) &&
              performanceState.observers.size >= workerPerformanceObserverLimit)
            throw new RangeError("PerformanceObserver quota exceeded");
          observerState.types = types;
          performanceState.observers.add(this);
          if (hasType && options.buffered && types.has(requested[0])) {
            for (const entry of performanceState.entries) {
              if (entry.entryType !== requested[0]) continue;
              if (observerState.records.length >=
                  workerPerformanceObserverRecordLimit) {
                observerState.records.shift();
                observerState.dropped++;
              }
              observerState.records.push(entry);
            }
            observerState.schedule();
          }
        }
        disconnect() {
          const observerState = workerPerformanceObserverStates.get(this),
            performanceState = workerPerformanceStates.get(scope.performance);
          if (!observerState || !performanceState)
            throw new TypeError("Illegal invocation");
          observerState.types.clear();
          observerState.records.length = 0;
          observerState.pending = false;
          observerState.dropped = 0;
          performanceState.observers.delete(this);
        }
        takeRecords() {
          const observerState = workerPerformanceObserverStates.get(this);
          if (!observerState) throw new TypeError("Illegal invocation");
          return sortWorkerPerformanceEntries(
            observerState.records.splice(0));
        }
      };
      Object.defineProperty(
        scope.PerformanceObserver, "supportedEntryTypes",
        { value: workerPerformanceEntryTypes, enumerable: true },
      );
      Object.defineProperty(
        scope.PerformanceObserver.prototype, Symbol.toStringTag,
        { configurable: true, value: "PerformanceObserver" },
      );
      markNative(scope.PerformanceObserver);
      for (const key of Reflect.ownKeys(scope.PerformanceObserver.prototype)) {
        const descriptor = Object.getOwnPropertyDescriptor(
          scope.PerformanceObserver.prototype, key);
        markNative(descriptor?.value);
      }
      scope.crossOriginIsolated = false;
      const scheduleWorkerTimer = (callback, delay, repeat, args) => {
          if (typeof callback !== "function") {
            const source = trustedString(callback);
            callback = () => runWorkerNative(
              state.realm, source, workerURL, 4);
            args = [];
          }
          const nesting = state.currentTimerNesting < 0
              ? 0 : state.currentTimerNesting + 1;
          let id = 0;
          const invoke = () => {
            if (!repeat && state.timers.delete(id))
              workerProgress.pendingTimers--;
            if (!repeat) state.timerNestingById.delete(id);
            if (!state.active || state.closing) return;
            if (traceWorkerLifecycle)
              traceWorkerNative("worker-lifecycle", {
                stage: "timer-fire", id: state.id, repeat,
                at: workerProgressClock.now(),
              });
            workerProgress.timerCallbacks++;
            workerProgress.lastId = state.id;
            workerProgress.lastTaskMs = workerProgressClock.now();
            const previousNesting = state.currentTimerNesting;
            state.currentTimerNesting = nesting;
            try {
              workerIntrinsics.apply(callback, state.scope, args);
            } catch (error) {
              reportWorkerError(owner, error, workerURL);
            } finally {
              state.currentTimerNesting = previousNesting;
            }
          };
          const requestedDelay = normalizeTimerDelay(delay),
            normalizedDelay = nesting >= 5
              ? Math.max(4, requestedDelay) : requestedDelay;
          id = repeat
            ? scheduleInterval(invoke, Math.max(1, normalizedDelay))
            : scheduleTimeout(invoke, normalizedDelay);
          if (id) {
            state.timers.add(id);
            state.timerNestingById.set(id, nesting);
            workerProgress.pendingTimers++;
            if (traceWorkerLifecycle) {
              const at = workerProgressClock.now();
              traceWorkerNative("worker-lifecycle", {
                stage: "timer-schedule", id: state.id, repeat,
                delay: normalizedDelay, at,
                due: at + Math.max(0, normalizedDelay),
              });
            }
          }
          return id;
        },
        clearWorkerTimer = (id) => {
          const timerId = Number(id);
          /* HTML timers are isolated by global object.  The worker uses the
             owner's scheduler underneath, so never forward an identifier
             until this worker has proved it owns that timer. */
          if (state.timers.has(timerId)) {
            cancelTimer(timerId);
            state.timers.delete(timerId);
            state.timerNestingById.delete(timerId);
            workerProgress.pendingTimers--;
            if (traceWorkerLifecycle)
              traceWorkerNative("worker-lifecycle", {
                stage: "timer-clear", id: state.id,
                at: workerProgressClock.now(),
              });
          }
        };
      scope.setTimeout = (callback, delay, ...args) =>
        scheduleWorkerTimer(callback, delay, false, args);
      scope.setInterval = (callback, delay, ...args) =>
        scheduleWorkerTimer(callback, delay, true, args);
      scope.clearTimeout = clearWorkerTimer;
      scope.clearInterval = clearWorkerTimer;
      scope.TextEncoder = TextEncoder;
      scope.TextDecoder = TextDecoder;
      scope.Blob = Blob;
      scope.URL = URL;
      scope.URLSearchParams = URLSearchParams;
      scope.Headers = Headers;
      const OwnerRequest = Request,
        WorkerRequest = class Request extends OwnerRequest {
          constructor(input, init) {
            /* A Worker has its own API base URL.  In particular a relative
               URL cannot be resolved against a blob: worker URL; it must not
               silently inherit the owning document's base URL. */
            if (!(input instanceof OwnerRequest)) {
              const text = trustedString(input),
                absolute = /^[A-Za-z][A-Za-z0-9+.-]*:/.test(text);
              if (
                workerURL.startsWith("blob:") &&
                !absolute
              )
                throw new TypeError("Invalid URL");
              /* An absolute input does not depend on the worker's base. The
                 URL parser used by the PSP intentionally cannot resolve a
                 relative reference against blob:, so passing that base even
                 for an absolute network URL incorrectly rejected every
                 fetch from a blob Worker. */
              input = absolute
                ? new URL(text).href
                : new URL(text, workerURL).href;
            }
            super(input, init);
          }
        };
      scope.Request = WorkerRequest;
      scope.Response = Response;
      /* Every worker fetch is tied to the worker's lifetime: terminate() and
         close() abort it. An author-supplied signal still aborts it too. */
      state.abortController = new AbortController();
      scope.fetch = (input, init) => {
        if (!state.active || !state.abortController)
          return Promise.reject(
            new DOMException("Worker is terminated", "InvalidStateError"),
          );
        let sourceRequest;
        try {
          /* Fetch reports Request-construction failures by rejecting its
             returned promise, rather than throwing them synchronously. */
          sourceRequest = new WorkerRequest(input, init);
        } catch (error) {
          return Promise.reject(error);
        }
        const authorSignal = sourceRequest.signal,
          controller = new AbortController(),
          abort = () =>
            controller.abort(
              authorSignal && authorSignal.aborted
                ? authorSignal.reason
                : state.abortController
                  ? state.abortController.signal.reason
                  : undefined,
            );
        if (authorSignal) {
          if (authorSignal.aborted) abort();
          else authorSignal.addEventListener("abort", abort, { once: true });
        }
        state.abortController.signal.addEventListener(
          "abort", abort, { once: true },
        );
        const lifetimeSignal = state.abortController.signal,
          cleanup = () => {
            lifetimeSignal.removeEventListener("abort", abort);
            if (authorSignal)
              authorSignal.removeEventListener("abort", abort);
          },
          recordFetch = (name, start, response) => {
            const end = scope.performance.now();
            appendWorkerPerformanceEntry(
              scope.performance,
              new PerformanceResourceTimingCtor(name, "fetch", {
                startTime: start,
                responseEnd: end,
                responseStatus: Number(response?.status) || 0,
                contentType: response?.headers?.get("content-type") || "",
              }),
            );
          };
        try {
          /* The first construction snapshots all observable input/init
             conversions.  This clone only substitutes the combined author
             and lifetime signal, preserving that exact URL and request. */
          const request = new WorkerRequest(sourceRequest, {
              signal: controller.signal,
            }),
            timingName = request.url,
            timingStart = scope.performance.now();
          return fetchForWorker(request).then(
            value => {
              cleanup();
              recordFetch(timingName, timingStart, value);
              return value;
            },
            error => {
              cleanup();
              recordFetch(timingName, timingStart, null);
              throw error;
            },
          );
        } catch (error) { cleanup(); return Promise.reject(error); }
      };
      scope.atob = atob;
      scope.btoa = btoa;
      scope.structuredClone = (value, options = undefined) =>
        cloneWorkerValue(
          value, state.cloneIntrinsics, options?.transfer);
      const workerTrustedTypesRealm = createTrustedTypesRealm();
      Object.assign(scope, workerTrustedTypesRealm);
      Object.defineProperty(scope, "__tilefinchTrustedScriptForEval", {
        configurable: false, enumerable: false, writable: false,
        value: workerTrustedTypesRealm.__tilefinchTrustedScriptForEval,
      });
      scope.queueMicrotask = callback => {
        if (typeof callback !== "function")
          throw new TypeError("microtask callback must be callable");
        queueMicrotask(() => {
          /* close() discards later tasks, not the current task's microtask
             checkpoint. Keep queueMicrotask aligned with Promise reactions;
             terminate() still makes state inactive and suppresses both the
             callback's observable Worker work and any later task. */
          if (state.active) callback();
        });
      };
      const workerLanguages = Object.freeze(
          Array.from(ownerNavigator.languages || []).slice(0, 8),
        );
      workerNavigator = Object.create(WorkerNavigator.prototype);
      workerLocation = Object.create(WorkerLocation.prototype);
      const workerPromiseFacade = (
          target, methodNames, facadePrototype, transformResult,
        ) => {
          if (!target) return undefined;
          const facadeTarget = Object.create(
              facadePrototype || Object.getPrototypeOf(target),
            ),
            wrappers = Object.create(null);
          let facade = null;
          facade = new Proxy(facadeTarget, {
            get(object, key, receiver) {
              if (!methodNames.includes(key))
                return Reflect.get(object, key, receiver);
              if (!wrappers[key]) {
                const method = Reflect.get(target, key, target);
                wrappers[key] = function (...args) {
                  if (this !== facade)
                    throw new TypeError("Illegal invocation");
                  const WorkerPromise = state.realm?.Promise;
                  if (typeof WorkerPromise !== "function")
                    throw new DOMException(
                      "Worker is terminated", "InvalidStateError");
                  try {
                    const result = WorkerPromise.resolve(
                      Reflect.apply(method, target, args),
                    );
                    return typeof transformResult === "function"
                      ? result.then(value => transformResult(value, key))
                      : result;
                  } catch (error) {
                    return WorkerPromise.reject(error);
                  }
                };
                markNative(wrappers[key]);
              }
              return wrappers[key];
            },
          });
          return facade;
        },
        workerNavigatorValues = {
          userAgent: () => String(ownerNavigator.userAgent || ""),
          appCodeName: () => "Mozilla",
          appName: () => "Netscape",
          appVersion: () => {
            const userAgent = String(ownerNavigator.userAgent || "");
            return userAgent.startsWith("Mozilla/")
              ? userAgent.slice(8) : "";
          },
          language: () => String(ownerNavigator.language || ""),
          languages: () => workerLanguages,
          platform: () => String(ownerNavigator.platform || ""),
          product: () => "Gecko",
          productSub: () => String(ownerNavigator.productSub || ""),
          vendor: () => String(ownerNavigator.vendor || ""),
          vendorSub: () => String(ownerNavigator.vendorSub || ""),
          onLine: () => !!ownerNavigator.onLine,
          hardwareConcurrency: () =>
            Math.max(1, Number(ownerNavigator.hardwareConcurrency) || 1),
          deviceMemory: () => Number(ownerNavigator.deviceMemory) || 0.25,
          userAgentData: () => {
            const ownerUAData = ownerNavigator.userAgentData;
            if (!ownerUAData) return undefined;
            if (workerUAData === null)
              workerUAData = workerPromiseFacade(
                ownerUAData, ["getHighEntropyValues"]);
            return workerUAData;
          },
          connection: () => ownerNavigator.connection,
          permissions: () => {
            if (workerPermissions === null) {
              const LocalPermissions = state.realm?.Permissions,
                LocalPermissionStatus = state.realm?.PermissionStatus,
                localizeStatus = value => {
                  if (!LocalPermissionStatus || value == null) return value;
                  const status = Object.create(LocalPermissionStatus.prototype);
                  Object.defineProperties(status, {
                    _name: {
                      configurable: true, writable: true,
                      value: String(value.name || ""),
                    },
                    _onchange: {
                      configurable: true, writable: true, value: null,
                    },
                  });
                  return status;
                };
              workerPermissions = workerPromiseFacade(
                ownerNavigator.permissions, ["query"],
                LocalPermissions?.prototype, localizeStatus,
              );
            }
            return workerPermissions;
          },
          storage: () => {
            if (workerStorage === null) {
              const LocalStorageManager = state.realm?.StorageManager,
                localizeStorageResult = (value, method) => {
                  if (method !== "estimate" || value == null) return value;
                  const result = Object.create(
                    state.realm?.Object?.prototype || Object.prototype,
                  );
                  result.usage = Math.max(0, Number(value.usage) || 0);
                  result.quota = Math.max(0, Number(value.quota) || 0);
                  return result;
                };
              workerStorage = workerPromiseFacade(
                ownerNavigator.storage, ["estimate", "persisted"],
                LocalStorageManager?.prototype, localizeStorageResult,
              );
            }
            return workerStorage;
          },
          gpu: () => {
            const ownerGPU = ownerNavigator.gpu;
            if (!ownerGPU) return undefined;
            if (workerGPU === null) {
              const LocalGPU = state.realm?.GPU,
                LocalWGSL = state.realm?.WGSLLanguageFeatures;
              workerGPU = workerPromiseFacade(
                ownerGPU, ["requestAdapter"], LocalGPU?.prototype,
              );
              /* requestAdapter must return a Worker-realm Promise, while the
                 synchronous SameObject capability set must also carry the
                 Worker's constructor identity. Its empty contents are the
                 honest PSP result. */
              if (LocalWGSL?.prototype) {
                const localWGSL = Object.create(LocalWGSL.prototype);
                Object.defineProperty(workerGPU, "wgslLanguageFeatures", {
                  configurable: true,
                  enumerable: true,
                  value: localWGSL,
                });
              }
            }
            return workerGPU;
          },
        },
        workerLocationValues = {
          href: () => workerLocationURL.href,
          origin: () => workerOrigin,
          protocol: () => workerLocationURL.protocol,
          host: () => workerLocationURL.host,
          hostname: () => workerLocationURL.hostname,
          port: () => workerLocationURL.port,
          pathname: () => workerLocationURL.pathname,
          search: () => workerLocationURL.search,
          hash: () => workerLocationURL.hash,
        };
      for (const name of Object.keys(workerNavigatorValues)) {
        const read = workerNavigatorValues[name];
        Object.defineProperty(WorkerNavigator.prototype, name, {
          configurable: true,
          enumerable: true,
          get() {
            if (this !== workerNavigator) throw new TypeError("Illegal invocation");
            return read();
          },
        });
      }
      for (const name of Object.keys(workerLocationValues)) {
        const read = workerLocationValues[name];
        Object.defineProperty(WorkerLocation.prototype, name, {
          configurable: true,
          enumerable: true,
          get() {
            if (this !== workerLocation) throw new TypeError("Illegal invocation");
            return read();
          },
        });
      }
      Object.defineProperty(WorkerLocation.prototype, "toString", {
        configurable: true,
        enumerable: true,
        writable: true,
        value() {
          if (this !== workerLocation) throw new TypeError("Illegal invocation");
          return workerLocationURL.href;
        },
      });
      Object.defineProperties(WorkerGlobalScope.prototype, {
        navigator: {
          configurable: true,
          enumerable: true,
          get() {
            if (this !== state.scope && this !== scope)
              throw new TypeError("Illegal invocation");
            return workerNavigator;
          },
        },
        location: {
          configurable: true,
          enumerable: true,
          get() {
            if (this !== state.scope && this !== scope)
              throw new TypeError("Illegal invocation");
            return workerLocation;
          },
        },
      });
      for (const prototype of [
        WorkerNavigator.prototype,
        WorkerLocation.prototype,
        WorkerGlobalScope.prototype,
      ]) {
        for (const name of Reflect.ownKeys(prototype)) {
          const descriptor = Object.getOwnPropertyDescriptor(prototype, name);
          markNative(descriptor?.value);
          markNative(descriptor?.get);
          markNative(descriptor?.set);
        }
      }
      /* Browser-owned platform objects are stable but extensible. Libraries
         may attach realm-local bookkeeping to navigator and location; only
         immutable value snapshots such as languages should be frozen. */
      /* The worker runs in its own realm (see createWorkerRealmNative).
         Only explicitly supported Web Platform names resolve, read-only, to
         the owner's implementations through this fallback deep in the
         prototype chain.  This keeps lazy constructor groups available while
         preventing owner-defined page state from entering the worker realm. */
      const fallback = new Proxy(Object.create(EventTarget.prototype), {
        has(target, key) {
          const present = key in target ||
            (typeof key === "string" && workerSharedGlobalNames.has(key) &&
              key in globalThis);
          if (!present) traceMissingWorkerCapability("has", key);
          return present;
        },
        get(target, key, receiver) {
          if (key in target) return Reflect.get(target, key, receiver);
          if (typeof key === "string" && workerSharedGlobalNames.has(key))
            return globalThis[key];
          traceMissingWorkerCapability("get", key);
          return undefined;
        },
        set(target, key, value, receiver) {
          return Reflect.defineProperty(receiver, key, {
            value,
            writable: true,
            enumerable: true,
            configurable: true,
          });
        },
      });
      Object.setPrototypeOf(WorkerGlobalScope.prototype, fallback);
      /* DedicatedWorkerGlobalScope.close(): messages the worker already
         posted in this task still reach the owner, further tasks queued to
         the worker (timers, owner messages) are discarded, and the shutdown
         itself runs once every task-0 timer queued before it has drained. */
      scope.close = () => {
        if (!state.active || state.closing) return;
        state.closing = true;
        /* Shut down once the closing task has ended and every message
           the worker posted (before or after close()) has reached the
           owner. Timer order alone cannot express that: a zero-delay
           timer scheduled during a drain may run in the same pass. */
        const closeTimer = scheduleTimeout(() => {
          state.closeTaskEnded = true;
          finishWorkerClose(state);
        }, 0);
        /* A full owner timer queue must not strand a closing Worker.  A
           microtask runs after the current Worker evaluation, avoiding realm
           destruction while its stack is still active. */
        if (!closeTimer)
          queueMicrotask(() => {
            state.closeTaskEnded = true;
            finishWorkerClose(state);
          });
      };
      scope.__tilefinchImportScripts = (...urls) => {
        if (!state.active)
          throw new DOMException("Worker is terminated", "InvalidStateError");
        if (workerType === "module")
          throw new TypeError("importScripts is unavailable in a module Worker");
        if (urls.length > 8)
          throw new RangeError("worker importScripts quota exceeded");
        for (const value of urls) {
          const importedReference = trustedString(value),
            importedURL = new URL(importedReference, workerURL).href,
            importedBlob = blobForURL(importedURL);
          if (!importedBlob && !/^https?:/.test(importedURL))
            throw new DOMException(
              "Worker script could not be loaded", "NetworkError");
          const importedSource = importedBlob
            ? workerSourceForBlob(importedBlob)
            : fetchWorkerScriptSync(importedURL, "cors", "same-origin");
          runWorkerNative(state.realm, importedSource, importedURL, 2);
        }
      };
      scope.__tilefinchWorkerType = workerType;
      scope.__tilefinchWorkerBlobBytes = globalThis.__tilefinchBlobBytes;
      scope.__tilefinchWorkerBlobText = blobTextForWorker;
      scope.__tilefinchOwnerWorkerPrototype = globalThis.Worker.prototype;
      scope.__tilefinchCreateNestedWorker = (url, options) => {
        const text = trustedString(url),
          absolute = /^[A-Za-z][A-Za-z0-9+.-]*:/.test(text),
          nestedURL = absolute ? new URL(text).href
            : new URL(text, workerURL).href,
          child = new globalThis.Worker(nestedURL, options);
        state.children.add(child);
        return child;
      };
      /* These two construction hooks exist only while the fresh realm builds
         its Worker-local XHR facade. The facade closes over their values and
         they are removed from the inherited scope before author code runs. */
      scope.__tilefinchRegisterWorkerXHR = (xhr) => {
        state.xhrs.add(xhr);
        EventTarget.prototype.addEventListener.call(
          xhr, "loadend", () => state.xhrs.delete(xhr));
      };
      scope.__tilefinchRegisterWorkerFileReader = (reader) => {
        state.fileReaders.add(reader);
        EventTarget.prototype.addEventListener.call(
          reader, "loadend", () => state.fileReaders.delete(reader));
      };
      scope.__tilefinchRegisterWorkerWebSocket = (socket) => {
        state.webSockets.add(socket);
        EventTarget.prototype.addEventListener.call(
          socket, "close", () => state.webSockets.delete(socket));
      };
      scope.__tilefinchGetOwnerIndexedDB = () => globalThis.indexedDB;
      scope.__tilefinchRegisterWorkerIDBRequest = (request, tracksDatabase) => {
        if (!tracksDatabase || !request ||
            typeof request.addEventListener !== "function") return;
        request.addEventListener("success", () => {
          let database = null;
          try { database = request.result; } catch (_) {}
          if (!database) return;
          if (state.active) state.idbConnections.add(database);
          else {
            try { abortIndexedDBForWorker(database); } catch (_) {}
          }
        }, { once: true });
      };
      scope.__tilefinchWorkerScriptURL = workerURL;
      state.realm = createWorkerRealmNative(
        scope, DedicatedWorkerGlobalScope.prototype,
        workerRealmConstructorInit);
      delete scope.__tilefinchRegisterWorkerXHR;
      delete scope.__tilefinchRegisterWorkerFileReader;
      delete scope.__tilefinchRegisterWorkerWebSocket;
      delete scope.__tilefinchGetOwnerIndexedDB;
      delete scope.__tilefinchRegisterWorkerIDBRequest;
      delete scope.__tilefinchWorkerScriptURL;
      delete scope.__tilefinchImportScripts;
      delete scope.__tilefinchWorkerType;
      delete scope.__tilefinchWorkerBlobBytes;
      delete scope.__tilefinchWorkerBlobText;
      delete scope.__tilefinchOwnerWorkerPrototype;
      delete scope.__tilefinchCreateNestedWorker;
      state.cloneIntrinsics = workerCloneIntrinsics.capture(state.realm);
      /* A fresh QuickJS global stringifies as [object global]; the worker
         global must present as its scope interface. */
      Object.defineProperty(state.realm, Symbol.toStringTag, {
        value: "DedicatedWorkerGlobalScope",
        configurable: true,
      });
      state.scope = state.realm;
        const retainedSource = blob ? workerSourceForBlob(blob) : null,
          evaluateWorkerSource = (source) => {
            if (!state.active) return;
            setCryptoBufferNormalizer(
              scope.crypto,
              runWorkerNative(
                state.realm,
                "value=>value instanceof ArrayBuffer?"
                  + "new Uint8Array(value):value",
                workerURL, 3,
              ),
              runWorkerNative(
                state.realm,
                "value=>new Uint8Array(value).slice().buffer",
                workerURL, 3,
              ),
            );
            runWorkerNative(
              state.realm, source, workerURL,
              workerType === "module" ? 1 : 0,
              workerType === "module" ? workerCredentials : "same-origin");
            workerProgress.startCompletions++;
            workerProgress.lastId = state.id;
            workerProgress.lastTaskMs = workerProgressClock.now();
          },
          failWorkerStart = (error) => {
            if (!state.active) return;
            workerProgress.startFailures++;
            reportWorkerError(this, error, workerURL);
          };
        /* Snapshot a retained Blob before revokeObjectURL can remove its URL.
           Network Workers instead use the ordinary bounded fetch scheduler,
           with worker-src/mixed-content/CORS policy applied at its native
           destination, and are tied to terminate()/close() through the
           Worker's lifetime signal. */
        state.startTimer = scheduleTimeout(() => {
          state.startTimer = 0;
          if (!state.active) return;
          if (traceWorkerLifecycle)
            traceWorkerNative("worker-lifecycle", {
              stage: "start-fire", id: state.id,
              at: workerProgressClock.now(),
            });
          try {
            if (retainedSource !== null) {
              evaluateWorkerSource(retainedSource);
              return;
            }
            fetchWorkerScript(workerURL, {
              mode: workerType === "module" ? "cors" : "same-origin",
              credentials: workerType === "module"
                ? workerCredentials : "same-origin",
              signal: state.abortController.signal,
            }).then(response => {
              if (!response || !response.ok)
                throw new DOMException(
                  "Worker script could not be loaded", "NetworkError");
              return response.text();
            }).then(evaluateWorkerSource, failWorkerStart);
          } catch (error) {
            failWorkerStart(error);
          }
        }, 0);
        if (!state.startTimer)
          throw new DOMException(
            "Worker startup task quota exceeded", "QuotaExceededError");
        if (traceWorkerLifecycle)
          traceWorkerNative("worker-lifecycle", {
            stage: "start-schedule", id: state.id,
            at: workerProgressClock.now(),
          });
      } catch (error) {
        activeWorkers--;
        workerProgress.startFailures++;
        if (state) state.active = false;
        if (state?.realm) {
          try {
            destroyWorkerRealmNative(state.realm);
          } catch (_) {}
          state.realm = null;
        }
        if (state) state.scope = null;
        workerStates.delete(this);
        throw error;
      }
    }
    get onmessage() {
      return workerStates.get(this)?.handlers.message.value || null;
    }
    set onmessage(value) {
      const state = workerStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      setEventHandler(this, state.handlers, "message", value);
    }
    get onerror() {
      return workerStates.get(this)?.handlers.error.value || null;
    }
    set onerror(value) {
      const state = workerStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      setEventHandler(this, state.handlers, "error", value);
    }
    get onmessageerror() {
      return workerStates.get(this)?.handlers.messageerror.value || null;
    }
    set onmessageerror(value) {
      const state = workerStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      setEventHandler(this, state.handlers, "messageerror", value);
    }
    addEventListener(type, callback, options = false) {
      const state = workerStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      return EventTarget.prototype.addEventListener.call(
        this, type, callback, options);
    }
    removeEventListener(type, callback, options = false) {
      const state = workerStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      return EventTarget.prototype.removeEventListener.call(
        this, type, callback, options);
    }
    postMessage(value) {
      const state = workerStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      /* HTML silently discards messages posted after terminate(). The Worker
         object remains a valid EventTarget; termination is not an invocation
         or serialization error. */
      if (!state.active) return;
      if (arguments.length > 1) workerProgress.inboundTransferArguments++;
      traceWorkerMessage("owner-to-worker", value);
      const transferOrOptions = arguments[1],
        transfer = transferOrOptions &&
            typeof transferOrOptions === "object" &&
            !Array.isArray(transferOrOptions) &&
            "transfer" in transferOrOptions
          ? transferOrOptions.transfer : transferOrOptions,
        ports = [],
        copied = cloneWorkerValue(
          value, state.cloneIntrinsics, transfer, ports);
      workerProgress.inboundQueued++;
      workerProgress.lastId = state.id;
      const queued = scheduleTimeout(() => {
        if (!state.active || state.closing) {
          workerProgress.inboundDropped++;
          return;
        }
        workerProgress.inboundDelivered++;
        workerProgress.lastId = state.id;
        workerProgress.lastTaskMs = workerProgressClock.now();
        const event = trustedEvent(createMessageEvent(
          "message", copied, "", null, ports));
        globalThis.__tilefinchPrepareEvent(
          event, state.scope, [state.scope]);
        let eventFinished = false;
        const finish = () => {
          if (eventFinished) return;
          eventFinished = true;
          globalThis.__tilefinchFinishEventDispatch(event);
          globalThis.__tilefinchRecordEvent();
        };
        try {
          if (!event.__stopped)
            invokeEventTargetCheckpointed(
              state.scope, event, Event.AT_TARGET, null, null, finish);
          else finish();
        } catch (error) {
          finish();
          throw error;
        }
      }, 0);
      if (!queued) workerProgress.inboundDropped++;
    }
    terminate() {
      const state = workerStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      shutdownWorkerState(state, 1);
    }
  };
  /* Shared by Worker.terminate() and DedicatedWorkerGlobalScope.close().
     Besides timers and listeners, abort every fetch the worker started so
     its continuations stop consuming the page's callback and network
     budgets after termination. */
  function finishWorkerClose(state) {
    if (state.closing && state.closeTaskEnded && !(state.outboundPending > 0))
      shutdownWorkerState(state, 2);
  }
  function shutdownWorkerState(state, reason = 0) {
    if (!state.active) return;
    if (traceWorkerLifecycle)
      traceWorkerNative("worker-lifecycle", {
        stage: "terminate", id: state.id, reason,
        at: workerProgressClock.now(),
      });
    state.active = false;
    activeWorkers--;
    workerProgress.terminations++;
    workerProgress.lastId = state.id;
    workerProgress.lastTerminationReason = reason;
    workerProgress.lastTaskMs = workerProgressClock.now();
    if (state.realm) {
      try {
        destroyWorkerRealmNative(state.realm);
      } catch (_) {}
      state.realm = null;
    }
    if (state.startTimer) {
      cancelTimer(state.startTimer);
      state.startTimer = 0;
    }
    for (const id of state.timers) cancelTimer(id);
    workerProgress.pendingTimers = Math.max(
      0, workerProgress.pendingTimers - state.timers.size);
    state.timers.clear();
    state.timerNestingById.clear();
    for (const xhr of state.xhrs) {
      try { abortXHRForWorker(xhr); } catch (_) {}
    }
    state.xhrs.clear();
    for (const reader of state.fileReaders) {
      try { abortFileReaderForWorker(reader); } catch (_) {}
    }
    state.fileReaders.clear();
    for (const socket of state.webSockets) {
      try { closeWebSocketForWorker(socket); } catch (_) {}
    }
    state.webSockets.clear();
    for (const database of state.idbConnections) {
      try { abortIndexedDBForWorker(database); } catch (_) {}
    }
    state.idbConnections.clear();
    for (const child of state.children) {
      try { child.terminate(); } catch (_) {}
    }
    state.children.clear();
    /* Termination stops Worker execution but does not stop the Worker object
       from being an EventTarget for synthetic author events. Keep its owner
       listener map until the object itself becomes unreachable. */
    if (state.abortController) {
      try {
        state.abortController.abort(
          new DOMException("Worker is terminated", "AbortError"),
        );
      } catch (_) {}
      state.abortController = null;
    }
    state.scope = null;
  }
  Object.defineProperty(globalThis.Worker.prototype, Symbol.toStringTag, {
    configurable: true,
    value: "Worker",
  });
  markNative(globalThis.Worker);
  for (const key of Reflect.ownKeys(globalThis.Worker.prototype)) {
    const descriptor = Object.getOwnPropertyDescriptor(
      globalThis.Worker.prototype,
      key,
    );
    markNative(descriptor?.value);
    markNative(descriptor?.get);
    markNative(descriptor?.set);
  }
};
