# Changelog

Newest first. Each release section is finalized from the Unreleased section
at cut time: the `## Unreleased` heading is renamed to
`## <version> — <date>` and a fresh empty `## Unreleased` section is added
above it. [docs/RELEASE_PROCESS.md](docs/RELEASE_PROCESS.md) describes the
mechanics.

## Unreleased

## 0.1.48 — 2026-11-11

- Revisited pages keep the same stylesheet ordering as a fresh load when later styles arrive; cached styles no longer bypass the safeguards for browser-added CSS.
- Deferred SVG parsing now counts its own retained memory accurately when other caches are reclaimed, keeping it within its existing allowance under memory pressure.
- Accepting the keyboard (the system keyboard's Done, or Start in Danzeff) in a form's only text field, such as a site's search box, now sends the form, as Enter does in other browsers: the page's submit handler runs and can stop it, and the form's default button is used when it has one. Before, the text was only filled in and Start had to be pressed again. Forms with more than one text field (a user name and a password) and multi-line text boxes still only fill in the text.
- Enter (START on a text field) in a form whose first submit button is disabled now submits nothing, as in Chrome. Before, the browser clicked the next enabled submit button instead, or submitted a form with one text field without its button.
- A track or queue menu opened while a video is opening (the next queue entry, or a rewind of 30 s or more that restarts the decoder) now stays open, on the row you highlighted, when the video finishes opening. Before, the menu closed at that moment, so a Cross meant for a row paused or played the video instead.
- If the browser's own stylesheet for a page (content-blocker hiding, cookie notices, Reader view, text size) is set while the page is still loading, the page now shows with it. Before, a page whose last `<style>` arrived after the change kept the stylesheet compiled without it, so the cookie notices and ads it hides could stay visible, and a later visit could start from that sheet.
- A page whose script disables one of its stylesheet links (`link.disabled = true`, or the `disabled` attribute) while it loads, or switches the theme its root element selects (`data-color-mode` with a `data-color-theme` list of theme sheets, as GitHub's pages have), now shows the styles that change leaves. Before, the page kept the stylesheet compiled before the change. Two pages of a site that differ only in that theme selection no longer share their compiled head stylesheet either.
- A page opened after night mode turns on or off (by hand, or by Auto at 19:00 and 07:00) now shows the colours its stylesheet gives that mode. Before, a page whose stylesheet the browser had kept from an earlier visit in the other mode (a site adapter's results or watch page, Back to a page, or a Wikipedia article sharing the previous one's head stylesheets) kept that mode's `prefers-color-scheme` rules, so a site with its own dark theme could show fully light at night. A page that is still loading when night mode changes now compiles its stylesheet again for the new mode instead of adding the rest of its styles to the part compiled for the old one.
- An external script whose response is empty now runs as an empty script and fires `load`, as in Chrome. Before, a classic script fired `error` (running the page's error handlers), a module script fired neither event, and a module that imported an empty module failed to load.
- When the content blocker refuses a second Google Tag Manager container that keeps its own data layer (`gtm.js?id=…&l=otherLayer`), the callbacks the page queues or pushes on that layer now run, as they do for the first container. Before, only the first refused container's layer was answered, so a link waiting on the second one's callback never navigated.
- On a PSP, when a page's loading preview appears but its first screen has nothing to read yet (bbc.com/news shows only its header, with the logo still being drawn and the first headline below the screen), the bottom of the screen now says **Preview ready  R to read**. Pressing R pages down to the text while the page keeps loading. The note goes away once the screen shows text or a picture, when you press R or Circle, and when the page finishes loading or you go to another page. A page whose first screen already shows text shows no note.
- Basic content blocking (the default) now also refuses the scripts of tag managers (Google Tag Manager and gtag.js, Adobe Launch, Tealium, Ensighten, Commanders Act, Segment), A/B testing services (Optimizely, VWO, AB Tasty, Google Optimize, Kameleoon, Convert, Adobe Target) when another site's page loads them. These scripts are large and run while the page loads, so news pages that use them load faster and use less memory. A page that hides itself until its A/B testing script answers is shown at once, and links that wait on a Google tag before they navigate still navigate. Paywall and subscription SDKs (Piano, Poool, Pelcro, LaterPay) are not refused, so a metered or subscription page counts reads and shows its paywall as the site intends. Setting the blocker to Off, or allowing the site under **Permissions & controls → Content blocking**, loads them again. The content-blocking reference lists the hosts.
- A stylesheet that a page adds ahead of stylesheets it already has (a `<style>` or `<link>` inserted before them, or a constructed sheet adopted again between others) no longer hands some later rules the selectors of the new sheet's rules. Before, elements could take styles meant for others, or lose their own, until something made the page restyle everything; Rotten Tomatoes' movie page laid out about 1,200 px shorter that way.
- A page that changes a `<style>` element by inserting or removing a text node in it (`style.appendChild(document.createTextNode(css))`, or removing its text child) now shows the change at once. Before, the page kept the old styles until something else made it restyle everything; archive.org's search page drew its text in the wrong grey that way while it loaded.
- On a PSP, a YouTube page that takes more than 30 seconds to arrive (a slow connection, a long watch page) now keeps loading while its data keeps coming, as web pages already did. Before, the loading watchdog counted no progress for YouTube pages and stopped every one 30 seconds after it started.
- On a PSP, a page opened from a link, a reload, or a YouTube results or watch page now gets more of each frame while it loads, so it appears sooner: a Wikipedia article about 4 s sooner (first screen at about 4 s instead of 8 s on the emulator), a YouTube watch page about 1.9 s sooner. Every frame still redraws the screen and reads the buttons, as before.
- Going Back to a YouTube page, or between the results and a watch page more than once, no longer compiles the page's stylesheet again on a PSP when it is the one the previous page left (about 60 ms each).
- Opening a YouTube page (results, a watch page, Back to one) on a PSP now lays it out once instead of twice when its only images at that point are its own icons, about 0.1 to 0.2 s less before it appears.
- On the YouTube watch page the channel avatar now loads just after the page appears, as the thumbnails do. Before, opening the page waited for it, which on a live network is a request to a second server.
- Pages with large stylesheets of utility classes (www.reddit.com) work out each element's style faster: for each of the less common properties the browser checks once which rules could apply to the element by its tag, id, classes and attributes, instead of testing every rule's selector. Styles are unchanged.
- `canvas.toDataURL()` and `canvas.toBlob()` now encode a picture many times faster, with the same bytes as before: the checksums a PNG needs are computed natively. Pages that call them while they load (old.reddit.com does three times, and many sites do to test emoji or image support) finish loading sooner on a PSP.
- A page whose first screen holds only pictures that have not arrived yet (a logo or icons in a header bar), with its first text further down, now shows while it loads instead of the loading screen until all of its scripts have run: bbc.com/news appears in a few seconds on a PSP instead of after about half a minute (in dark mode its header and the grey band below it turn one colour, so it still waits for its text). The pictures appear as they are drawn, and the text below can be paged to at once. A first screen that would still be blank until its pictures arrive (one large picture, or a grid of pictures on the bare page background) waits as before.
- While a video is opening (the next entry of a queue, or a rewind of 30 s or more that restarts the decoder), each button press now acts once. Before, one Down in the queue or track menu could move two rows, so Cross picked the entry or track below the one you meant. A queue entry or track chosen while it opens is carried out when the open ends, even if you press Left, Right or Cross on the timeline after choosing it.
- On a page that has already run a lot of script, a later script is no longer skipped for memory while there is still room to compile it. The browser counted the text of the scripts it had already run twice when deciding whether the next one fits.
- `document.append(document.documentElement, document.documentElement)` (and the same `prepend`) on the page now leaves the document as it was, as in Chrome, and any other call that would give the page's document a new or moved child throws before an argument moves. Before, the page lost its root element.
- An element whose local name holds a colon, as `new Document().createElement("a:b")` or an XML document's `createElement` makes, can now be put into the page and keeps that name, as in Chrome. Before, the insertion threw `NamespaceError`.
- A refused `append`, `prepend` or `replaceChildren` on a document now empties a lone `DocumentFragment` or shadow-root argument, and given several arguments takes a node out of another document even when it is that document's root, as Chrome does. Before, a lone fragment or shadow root kept its children, and another document's root stayed in that document.
- `createElementNS("http://www.w3.org/1999/xhtml", name)` with capitals in the name (`SCRIPT`, `DIV`, `Foo`) now makes an `HTMLUnknownElement`, as in Chrome: a `SCRIPT` no longer runs, a `DIV` is no longer laid out or matched as a `div`, and none matches a lower-case selector or `getElementsByTagName` name. Before, such an element reported its name with capitals but behaved as the lower-case HTML element. An element made with a prefix in that namespace (`h:div`) is now the element its local name says (an `HTMLDivElement`), no longer an `HTMLUnknownElement`.
- An SVG element whose name has capitals (`linearGradient`, `clipPath`, `foreignObject`), or an element `createElementNS` made with a name such as `Foo` or `p:Foo`, is now found by its own name as in Chrome: `querySelector`, `querySelectorAll`, `matches` and `closest` match such an element's name in any case, and `getElementsByTagName` matches its exact name (`getElementsByTagName("linearGradient")` finds it, `"lineargradient"` does not). Before, these lookups found it only by its name in lower case, and none found an element by the `localName` or `tagName` it reports.
- A page whose own script removes `<body>` while the page loads (`document.body.remove()`) now finishes loading and shows what is left (with nothing but the head, an empty page), as in Chrome. Before, the whole load failed with "layout construction failed".
- While a video is opening (the next entry of a queue, or a rewind of 30 s or more that restarts the decoder), each button press now acts once. Before, one Down in the queue or track menu could move two rows, so Cross picked the entry or track below the one you meant.
- A page's external, module and inserted scripts that are too large to compile in the memory the page has left are now skipped before compiling, as inline ones already were. Before, the browser counted the same free memory twice for them, so it spent the time compiling such a script (seconds on a PSP) only to run out of memory. Scripts that fit are run as before.
- On a PSP, a page's own script that it loads before showing the rest of the page (or runs once the page has arrived) no longer waits out its timeout and fails when the page's images and other requests hold every connection: like a synchronous XHR or a module import, it now takes the connection kept for requests a script waits on. Scripts a page adds itself still share the ordinary connections.
- A script a page writes with `document.write()` from one of its own scripts while it loads (as ad, analytics and widget loaders do) now runs, in the order other browsers use: a written inline script runs at once, a written `<script src>` runs as soon as the writing script finishes and before anything written after it, and the written markup now appears right after the writing script instead of at the end of the page. Written scripts are checked by the content blocker and the page's Content-Security-Policy like any other. A page can have more scripts to run than before, which costs memory and time on pages that rely on this; `document.write()` from a timer, an event or an asynchronous script still adds its markup to the end of the page without running its scripts.
- On a PSP set to reopen the last page or a custom homepage at start, the first page now starts loading about 0.1 to 0.2 s sooner after Wi-Fi connects. The security setup that every secure connection needs now runs while the browser waits for Wi-Fi, as it already did when starting on Home; what it collects and how much it requires are unchanged.
- A synchronous XHR or a module import no longer waits out its timeout (or, for an XHR, connects on the thread that draws the screen) when the page's images and other requests hold every connection: one connection and one request slot are now kept for requests a script waits on. A synchronous XHR that still finds the page's response memory full after 2 s now fails instead of connecting on that thread. On a PSP a page loads at most five resources at once instead of six (three instead of four while a video plays).
- On a PSP, if the network transport could not start or a page's request could not get its 64 KiB transfer buffer, the request now fails with that reason ("network transport worker unavailable") instead of connecting on the thread that draws the screen, and the next request tries the transport again (a failed start at most once a second). One refused buffer no longer moved every later request of that page onto that thread.
- A synchronous XHR or a module import made while every request slot the page has holds a finished response no longer freezes the page first (for 2 s, an import for its whole timeout) waiting for room that cannot free. Waiting for the page's response memory to free no longer keeps the PSP's processor busy, which slowed the very transfers it was waiting for.
- On a PSP, a queued YouTube video download no longer freezes the page you are reading while it looks up the video, which could take up to 30 seconds: the lookup now runs in the background the way it does for playback, and the page keeps scrolling. SAVING DOWNLOAD  O PAUSE, pausing and the failure messages are unchanged. When the browser already holds a YouTube identity from the watch page, the lookup reuses it and needs one request instead of up to five.
- On a PSP, answering a ZIP install, file download or offline-app sheet opened while a page was still loading no longer lets the loading screen go on drawing the sheet from memory already released, which could show garbled text or crash.
- A large SVG background inside a box that scrolls on its own (a scrolling panel within the page) is now kept in memory by where that box shows it, not by where it sits in the page: one the box scrolls into view is drawn ahead of time like one the page scrolls to, and one the box scrolls far out of view is let go.
- A large SVG background near the top of a page is now read once before it first appears instead of twice: the reading done to learn its size is kept until the page is laid out and drawn from. Reading the graphic is most of the cost of showing it on a PSP. The kept reading is let go as soon as the page is laid out, or at once for a graphic far down the page.
- A large SVG background far down a page is no longer drawn into memory until you scroll near it, and is let go again once you scroll far away. On CNN's home page this frees the 0.9 to 1.8 MB that a placeholder graphic 17 screens down used to hold from the first paint. The graphic is drawn once, just before it reaches the screen, and looks as before.
- Basic ad blocking now also blocks two ad-verification services, `confiant-integrations.net` and `btloader.com`, which pages such as Nintendo Life, Eurogamer and IGN load beside the ads Basic already blocks. In replays of those pages on a computer the most memory each uses falls by 4 to 9 MB (Nintendo Life: 25.2 MiB to 16.1 MiB) and they look the same.
- A large inline script now compiles from the page's own copy of its text instead of a second one, so it needs that much less memory while it compiles (1 MB less for a 1 MB script).
- A page with an inline script too large to compile in the memory the page has left (CNN's home page has one of 2.2 MB) now skips it as it arrives. Before, the browser kept the whole script and spent the time compiling it (seconds on a PSP) only to run out of memory, so such a page now shows sooner and peaks lower (CNN's home page, replayed on a computer: first paint about 0.1 s sooner, peak memory down from the full 32 MiB to 29.7 MiB). Scripts that fit are run as before.
- While a video plays, the page's own network work is paused. A synchronous XHR the page makes then now fails at once instead of running anyway, which on a PSP connected on the thread that draws the screen.
- Page scripts can no longer make their own events look like real user input (`isTrusted`), which the browser's internal event helper allowed.
- A page can no longer corrupt the browser's memory by removing an element from inside code the browser calls while it is still working on that element (such as a `toString` method passed to `setAttribute`); the element now stays intact until that call finishes.
- When a page removes its fullscreen element, the page now hears that fullscreen ended just after the removal, as in other browsers, rather than in the middle of it, where page code could crash the browser.
- A canvas that has drawn an image from another site now keeps its pixels inside the browser, out of reach of page scripts entirely, so a page that tampers with built-in JavaScript functions can no longer catch those pixels on their way through the browser's canvas code. Such a canvas draws exactly as before, and filling it with a pattern or a clipped rectangle is faster. Creating a pattern from such an image no longer marks the canvas that created it; painting with it marks the canvas painted, as in other browsers.
- Pages can no longer read the pixels of an image from another site through a canvas: the browser's own canvas code no longer hands them to page script, a page cannot make a canvas look untouched by faking the browser's answer, a canvas stays protected after its size is reset (as in other browsers), and a pattern made from such an image protects every canvas it paints.
- On a PSP, a WebGL scene with many drawn copies (instancing) is sent to the graphics chip in parts once its command list fills, instead of writing past the list into memory the chip then ran as commands.
- After an update that did not finish its first start, choosing START CURRENT VERSION (O) on the TILEFINCH SAFE START screen, or letting its 10 seconds run out, now asks only once. Later starts take the ordinary fast start instead of waiting on that screen every time, and **Settings → Updates → Version and update** says UPDATE DID NOT FINISH STARTING with X TRY AGAIN (restart to retry the update) and Square Discard. The downloaded update stays on the Memory Stick until you choose. The page offers the same choice with launchers installed before this change, which still show the screen at every start until one is chosen.
- On a nearly full Memory Stick, saving site data at exit now needs only the space the data takes. Before, it needed the full cache setting plus 5 MB however little was stored, so a few kilobytes of localStorage were not saved and MEMORY STICK FULL - SITE DATA NOT SAVED appeared at every exit.
- A custom ad-blocking list with a very long line (as published lists have for cosmetic and per-site rules) or more than 4,096 rules now loads: long lines are skipped and counted as ignored, the first 4,096 rules are kept, and the status adds LIST TOO LONG. Before, the whole list was refused and the previous one stayed active.
- A Memory Stick read error in an installed language or emoji pack no longer stops every other character from that pack loading, and is no longer retried on every frame; after repeated errors the pack's characters fall back as if it were not installed until the next start.
- Asking a language-pack offer for its size no longer creates (and leaves behind) empty folders for the pack on the Memory Stick; the folders are made when the download starts. A Memory Stick that refuses the folder now shows FONT PACK FOLDER COULD NOT BE CREATED when the download starts.
- A Memory Stick read error while loading settings at startup no longer deletes the newest copy of your settings, bookmarks and history; the older copy is used for that start and the newer one is kept.
- Deleting a downloaded file no longer leaves the whole file on the Memory Stick, invisible and never cleaned up, when power is cut at the wrong moment; the next start finishes the deletion.
- The offline library opens on a full or write-protected Memory Stick after a download was interrupted. Before, it said OFFLINE LIBRARY UNAVAILABLE until the card had space again, so nothing saved could be opened or deleted to make that space.
- Starting an older version under **Previous versions** no longer destroys data a newer version saved in a newer format. The offline library says OFFLINE LIBRARY UNAVAILABLE - UPDATE TILEFINCH (in all 13 interface languages, translations v16) instead of falling back to an older copy of its list and deleting the downloads only the newer list named; the saved cache and localStorage are left as they are instead of being replaced at exit; and a site's storage kept on the Memory Stick is left alone. Everything is there again once the newer version runs. A site whose storage file name collides with another site's can no longer delete or overwrite that site's file.
- A page's write to storage kept on the Memory Stick now fails at once when the card is full or the write fails, instead of seeming to succeed; before, the lost write also made the next start drop every value saved after it. A card read error while loading a site's storage no longer discards the rest of its saved values.
- Clearing storage for a site kept on the Memory Stick (**Clear local storage**, or a page's own `localStorage.clear()`) now removes the values from the card too. Before, the values stayed readable in the site's file, which also grew, and the earlier session's sessionStorage stayed in an always-kept site's file.
- With "Save RAM storage at exit" on, a start where saved localStorage could not be read (a card read error, or too little memory) no longer ends with an empty store saved over it at exit, which pushed the saved data out for good one exit later. The saved file is kept for the next start; localStorage written during that session is not saved.
- Holding L at startup, or the update recovery screen, now also deletes the faster-startup copy of the browser, so a damaged copy that hangs at start no longer does so again on every ordinary start after a safe start. The next ordinary start rebuilds it.
- Installing an update can no longer replace the version you are running. After starting with L held (which runs the previous version), or when the weekly check ran while a new version was still on its first start, the Update screen now says RESTART TO UPDATE instead of overwriting the running build and, on a second try, deleting the last copy of the version that worked. The check also no longer offers the version that just finished its first start as new.
- Holding L at startup no longer starts an update that is staged, failed or was rejected. Once an update is staged into the previous version's slot there is no previous version until the update's first start succeeds; until then L starts the current version (or the recovery screen after an update that did not finish starting).
- Files left by a failed update install no longer count against the free-space check for the next download, and leftovers too large for one pass are removed in rounds instead of failing every later install with STAGING DIRECTORY IS NOT CLEAN.
- A live feed (`EventSource`) that sends a lot at once no longer fails with an error while the page is busy, for example during a synchronous request in a message handler: the rest of the feed waits until the page has read what it holds. Before, more than 64 KiB arriving before the page read it ended the feed, and bytes that arrived during a message handler could be lost.
- A page whose `Content-Security-Policy` allows its live-update server with `connect-src wss://...` or `ws:` can now open its WebSocket. Before, every such socket was refused: only `'self'` or an `https://` source let it connect.
- An address written as a number in another form, such as `http://127.1/` or `http://0x7f.0.0.1/`, is now the same site as its usual dotted form (`http://127.0.0.1/`) for cookies, saved site settings and security checks, and two devices whose odd spellings end alike (`192.168.0x1.1` and `10.9.0x1.1`) are no longer taken for one site.
- A public page can no longer reach multicast and broadcast addresses (224.0.0.0 and up, where routers and media devices on your home network answer discovery requests), as it already could not reach private and loopback addresses.
- On a PSP, live updates (`EventSource` feeds, streamed answers, pages whose server sends the top of the page first) now reach the page within a fraction of a second instead of waiting until 48 KiB had arrived or the connection ended. A stream that keeps sending is no longer cut at the request's 30-second deadline, and a request whose server takes more than 6 seconds between its headers and its body no longer fails with "background response made no progress".
- On a PSP, a page downloading several things at once now receives them in the same small portions per frame as on other builds, instead of up to 48 KiB from each download in one go, which held up input and drawing between frames.
- A page request that waits for another site's permission (a CORS preflight), such as an analytics call to a slow third-party server, no longer holds up the page's other `fetch` and XHR requests until that site answers, and the browser no longer re-checks the waiting request 100 times a second meanwhile.
- On a PSP, a page request with a body of 8 KiB or more (a long form post or a chat request with its history) or with 2 KiB or more of its own headers no longer freezes the screen while it connects: it now runs on the network worker like any other request. Requests that carry more than about 2 KiB of cookies no longer fail on a PSP.
- A cross-site frame, such as an ad or a widget, no longer sends the page's own `SameSite=Lax` and `SameSite=Strict` cookies when it requests the page's site, so those cookies protect against cross-site request forgery from inside the page as in other browsers.
- Scripts that a page adds with `document.createElement('script')` now enforce their `integrity` attribute, as scripts in the page's HTML do: a script whose bytes do not match does not run and fires `error`, including from the cache. Before, its bytes ran unchecked, and under a hash-only `Content-Security-Policy` the attribute alone let any URL run. Such a script with `crossorigin` is now fetched with CORS.
- Closing a WebSocket while the server is already closing it, or while a message is still being sent, now ends with the clean close the server sent. Before, the page got an `error` and code 1006, and a socket closed this way by a terminating Worker could stay open until the page was left.
- WebSockets stay open after connecting and exchange messages. Before, every connection on the PSP closed with code 1006 right after its `open` event.
- Pages whose style sheets use the `~` sibling selector over long runs of sibling elements lay out faster: each element now learns whether an earlier sibling matches from the one before it instead of looking back through every earlier sibling (4,000 paragraphs took 82 ms of layout on a computer, now 12 ms).
- SVG masks that set `mask-type: alpha` in a `style` attribute followed by another declaration or `!important` now mask by alpha instead of hiding their content.
- An inline SVG icon whose `<use>` names an id that also appears elsewhere on the page now draws its own symbol instead of disappearing, and pages with many icons and a sprite at the end spend far less time resolving them.
- Scrolling inside a box on a page (a code block, a wide table, a carousel) no longer redraws the whole screen from scratch at every step after the first.
- Pages with very long unbroken lines, such as minified code or JSON shown as text, scroll far faster: drawing a line now stops just past the edge of the screen instead of going through the whole line for every part of the screen it crosses (a 50,000-character line took 31 ms per frame on a computer, now under 1 ms).
- Long pages with a fixed header, a cookie bar or sticky headings scroll with less work per frame: drawing them no longer looks through every drawing step of the page on every frame.
- Numbered lists written as `<ol reversed>` now count down; only the rarer `<ol reversed="reversed">` did before, and a plain `reversed` list counted up from 1.
- Long numbered lists, such as Wikipedia reference lists, lay out faster: each item now counts on from the item before it instead of recounting the list from its start. A list item numbered past 2147483647 (or below its negative) stays at that number.
- Forms with long drop-down lists, such as country or year pickers, lay out faster: each choice in a list no longer looks through the whole list to learn whether it is the one shown (a 4,000-entry list took about 1.3 s on a computer, now under 5 ms).
- Pages with many large pictures settle sooner after loading: shrinking each picture's memory to its drawn size no longer redraws the visible screen and the fixed header for every picture, and lazy pictures waiting just below the screen no longer make the browser search the whole page for pictures on every idle moment.
- Long pages that clip their content, for example with `html, body { overflow-x: hidden }` or `overflow: hidden` on every card or comment, no longer redraw all of that content on every frame once they pass about 5,000 drawing steps: moving the focus and scrolling copy finished tiles as on other pages.
- Pages with many bordered boxes, such as Wikipedia tables with a border on every cell, lay out faster: drawing a box's square border no longer touches every earlier box and link on the page.
- Scripts that find an element's position on the page by adding up `offsetTop` values get the same answer after the page scrolls, so "jump to" links, sticky headers and lazy loaders that use it land in the right place.
- Pages no longer see the browser's own bookkeeping attributes (`data-tilefinch-*`) in their markup, attribute lists or change observers, so sites that compare or rebuild the page from its HTML no longer find attributes the server never sent.
- Text selected or measured with a range that spans several elements now reads as exactly that text, not the whole paragraph around it.
- A hidden control (`display: none` on it or a parent) no longer takes focus from script, so pop-up menus and dialogs that move focus to their first visible field land on it.
- Text boxes and check boxes report their defaults (`defaultValue`, `defaultChecked`), so pages that warn about unsaved changes no longer think every form was edited.
- Older form scripts work: `document.forms.login.user.value`, `form.email`, `form[0]` and `form.elements.plan.value` (the checked radio button) find their controls, and a drop-down that script refills with `select.options.length = 0` and `options[n] = new Option(...)` now changes. `select.remove(0)` removes the first option instead of the whole drop-down.
- Documents that scripts build for themselves now work like the page's own: `DOMParser` keeps a parsed page's `<head>`, title and `<html>`/`<body>` attributes, so libraries that fetch the next page and swap it in (Turbo, pjax, Swup) see its title and assets, and `createHTMLDocument` documents (used by jQuery and sanitizers) support every selector, element query and `innerHTML`. SVG text parses into a real SVG; other XML, which Tilefinch cannot parse, gives a document with a `parsererror` root instead of an empty one. Changing such a document's own children follows the DOM standard's rules, as Chrome does: `replaceChildren`, `append`, `prepend`, `appendChild`, `insertBefore` or `replaceChild` that would leave it with text at the top, a second root element or a second doctype throws `HierarchyRequestError` and inserts nothing; given several arguments, such a call first takes each argument out of where it was, as Chrome does, so a refused `append(document.documentElement, text)` leaves the document without its root element. The same holds for documents from `createDocument`, `new Document()` and an empty `<iframe>`, which before could lose their root element to a refused `replaceChildren`, swap it for text through `replaceChild`, or take text through an iframe document's `appendChild`. Elements made in a `createDocument` XML document now keep their case and, from `createElement`, have no namespace, as in Chrome: `createElement("Foo")` is `Foo`, no longer an HTML `FOO`, and `createElementNS` keeps a prefix (`p:Root` has `prefix` `p` and `localName` `Root`). `new Document()` now makes an XML document as well, as in Chrome (before, an HTML one): its `createElement("Foo")` is a `Foo` with no namespace, and its `contentType` is `application/xml`. On the page's own document, `createElementNS` now keeps the name's case too (`createElementNS(null, "Foo")` is `Foo` with no namespace, no longer `foo`), elements keep a prefix (`prefix`, and a `localName` without it), SVG elements such as `linearGradient` report their `localName` in its proper case, and copies made by `cloneNode` keep the name. Element and attribute names are now checked as the DOM standard and Chrome check them: `createElement("1bad")`, `createElementNS(ns, "a b")`, `setAttribute("a=b", …)`, `toggleAttribute`, `createAttribute` and their namespaced forms throw `InvalidCharacterError` for a name that is not allowed (before, `createElement` and `createAttribute` took any name), and `createElement("a:b")` makes an element whose `localName` is `a:b`, with no prefix. The page's own `document` now has `replaceChildren`, with the same checks (text, a second root element or a second doctype throws `HierarchyRequestError`); `document.replaceChildren()` empties the page as in Chrome, but a call that would add a new node to the page's document throws, as `appendChild` there already does. Removing the page's doctype now leaves `document.doctype` `null`.
- Long forms keep what script puts in them: a select with more than 128 options follows `select.value`, a birth-date row of three selects keeps all three choices, and a form with more than 128 fields filled by script no longer submits the early ones empty. Reading which options are selected (form submission, pressing a select) no longer slows down with the square of the option count, and the browser's own record of a selected option no longer reaches a page's `MutationObserver`.
- While the PSP connects to Wi-Fi, the status line now names the step it is on (for example waiting for an IP address), as intended; it used to stay on "CONNECTING NETWORK" for the whole wait.
- Typing an address, a Find query or the developer URL while a page is still loading now works like typing at any other time: the keyboard no longer flickers away every tenth of a second, a Circle (a letter or backspace) no longer stops the load, and keys pressed in the keyboard no longer also act on the page after it closes. The time spent typing no longer counts toward the load's stall timeout.
- Choosing a Wi-Fi profile, the developer URL or the video decoder in Settings now changes only that line of `boot.cfg` (or `data/boot-overrides.cfg`). Before, the save rewrote the file with six keys and dropped every other setting you had added by hand, such as `file_kb=`, `app_heap_mb=` or `module_cache_dir=`, and its comments. If the file is there but cannot be read (a Memory Stick read error), the save now leaves it alone and shows "... NOT SAVED" instead of replacing it with only the saved line.
- A `boot.cfg` whose last line has no newline is now read; before, the whole file was ignored without a message and the browser started with an older copy or the defaults.
- If the optional software video decoder fails to load or start because memory is short, a later video tries it again (up to three times) instead of reporting "optional video decoder is unavailable" until the browser is restarted.
- A YouTube or page video that starts its pictures from a recovery point no longer stops when the first picture kept for reference after it is a scene cut, a reference B picture or a picture split into several slices.
- If a video's picture copy never finishes ("VIDEO STAGING TRANSFER DID NOT COMPLETE"), the message now appears once: Close closes it, Retry plays again, and later videos play instead of failing until the browser is restarted.
- A High-profile video played by the optional software decoder now stops with "unsupported H.264 stream" when the stream switches part-way to interlaced coding, another colour format or a higher bit depth, instead of handing the decoder pictures it was never built for.
- `document.adoptedStyleSheets` (and a shadow root's) is now the same live array on every read, so `adoptedStyleSheets.push(sheet)`, `splice` and index writes apply the sheet as in other browsers; before, they changed a copy and did nothing. A list may name the same sheet twice, as the CSSOM standard allows. `sheet.insertRule("}{")` and other unbalanced rule text now throws `SyntaxError` instead of inserting it.
- `performance.measure(name, "start")` no longer fails with "The mark start does not exist" on pages that load many resources or make many marks: marks (256), measures (64) and resource timings (128) each keep their own buffer, instead of sharing 128 entries in which resource loads pushed the marks out.
- Older event code works again: `document.createEvent("MouseEvents")` events have `initMouseEvent`, and `initUIEvent`, `initKeyboardEvent` and `FileReader.readAsBinaryString` exist. `new Event("x", null)` (and every other event constructor given `null`) now makes a default event instead of throwing.
- `FileReader.readAsDataURL` builds its base64 text natively (256 KiB took 6.3 ms on the host before, 0.12 ms now; PSP timing not yet measured), and a page that replaces `window.btoa` no longer changes what it returns.
- `TextEncoder.encodeInto`, which WebAssembly pages built with wasm-bindgen call for every string they pass in, now encodes natively instead of one code point at a time in script (4 KiB of text took 1.7 ms on the host before, about 0.01 ms now; PSP timing not yet measured).
- Building a long query through `url.searchParams.append` no longer re-encodes and re-parses the whole query on every call: each append now adds only its own pair (200 appends took 57 ms on the host before, 0.4 ms now; PSP timing not yet measured).
- An import map added after the page loaded, through `innerHTML` or a script, no longer redirects the page's later `import()` calls; only maps in the page's own markup count, as in other browsers. Before, markup injected into a page that could not run script could still reroute its modules.
- A page can no longer make `location.origin`, `window.origin`, its `blob:` addresses or its request referrers claim another site by redefining `URL.prototype.origin` or planting properties on `Object.prototype`; Tilefinch reads these from its own URL records. Before, a page could, for example, send a request whose referrer named an unrelated site. WebSocket message events now report the socket's own `wss://` origin, as in other browsers.
- `element.style.display = null` (and `setProperty(name, null)` or `style.cssText = null`) now removes the declaration, as in other browsers. Before, a page that hid an element and later cleared the style with `null` left it hidden, and other properties stored the text `null`.
- Prices and numbers on pages format as in other browsers: `Intl.NumberFormat` currency formats show `$1,234.50`, `€3.00` and `¥500` with each currency's usual decimals (before: `USD 1,234.5`), percentages round to whole percents, and compact (`1.2K`), scientific, significant-digit and sign options work. `(1234567).toLocaleString()` now groups digits (`1,234,567`), and `toLocaleString`, `toLocaleDateString` and `toLocaleTimeString` honour their options, such as `{style: "currency"}` or `{timeZone: "UTC"}`, which they used to ignore. Formatting stays US English for every locale.
- Page streams no longer fail when a step queues many pieces at once: a text splitter turning one download chunk into more than 64 lines (common for live feeds, NDJSON and CSV) used to stop with "stream queue limit exceeded". Streams now hold as much as the page's memory allows and report backpressure through `desiredSize`, as in other browsers.
- Style sheet rules now work as in other browsers: each entry of `sheet.cssRules` is a real `CSSStyleRule`, `CSSMediaRule`, `CSSKeyframesRule` and so on, with a `style` declaration you can read and change (for example a page reading its `:root` custom properties), nested `cssRules` for `@media` and `@supports`, and `CSSRule.STYLE_RULE`-style constants. Before, rules were plain text records without `style`, which broke CSS-variable ponyfills and theme scripts. Changes made through a rule (a nested `insertRule` or `deleteRule`, `selectorText`, or the rule's `style`) are held to the same 256 KiB per sheet as the sheet's own `insertRule`, adopted or not, and one that does not fit throws `QuotaExceededError` and leaves the rule, its `style` and the applied sheet as they were.
- `new URL()` now accepts `mailto:`, `tel:`, `data:`, `about:`, `javascript:` and other non-web addresses, plus `ws:` and `wss:` socket addresses, as other browsers do; before, a page that read every link with `new URL(link.href)` stopped at its first email or phone link. Addresses with surrounding spaces (common in copied attributes) are no longer taken as relative paths.
- jQuery selectors such as `$("li:first")`, `$("tr:first td")` and `$("input[type!=hidden]")` find their elements again: `querySelectorAll`, `matches` and `closest` now reject every selector that is not valid CSS (and the few valid forms Tilefinch cannot match, such as namespace prefixes) with a `SyntaxError`, as other browsers do, so jQuery falls back to its own engine instead of getting an empty result.
- Like, Subscribe, **Save to playlist** and the signed-in YouTube feeds send their request at once when the same account has used one of them in the last 30 minutes; before, each first downloaded the whole YouTube home page (up to 2 MiB) to read details Tilefinch already had.
- On a PSP set to a language other than English, an age-restricted YouTube video now says it needs age confirmation, and a paid title says it needs payment, as in English; before, the first read as a plain sign-in request and the second as "YouTube refused it", because only the English refusal text was understood.
- YouTube videos play on a PSP whose clock runs hours ahead, as with a time zone set for another continent; before, every video could fail with "media URL expires too soon" because the time YouTube gives its video links was compared with the PSP clock.
- YouTube captions in Cyrillic, Greek, Arabic, Hebrew, Thai, Devanagari and other non-Latin scripts now show both rows in full: a caption was cut after 159 bytes, often in the middle of a letter. A long auto-generated caption line, whose per-word timing marks made it over 512 bytes, is now shown instead of dropped.
- YouTube search, **Playlists** (including **Save to playlist**) and **History** now reach every entry YouTube sends: **Load more** continues with the 13th entry of the same answer before asking YouTube for the next one. Before, entries after the 12th of each answer were skipped, so a 13th playlist could not be opened or saved to.
- On a PSP set to a language other than English, a YouTube video now starts with one player request, as it already did in English; before, every play and every pre-resolve tried up to three player clients because Tilefinch's own language cookie made it forget the YouTube identity it had just read.
- A page can no longer take over the browser's own Worker setup, which let it run code from text under a security policy that forbids `eval`.
- Pages can no longer transfer or detach a WebAssembly memory's buffer (`transfer()`, `structuredClone` and `postMessage` transfer lists), as in other browsers. On a PSP such a buffer could read and write freed memory once the WebAssembly memory grew.
- Fixed a crash, or a request that never finished, when a page cancels one request from inside another request's completion handler.
- A long site address now gives way at its start, never its end: the address bar, Wi-Fi sign-in bar, Site information, Permissions & controls, tabs, Home, Site data & storage, the storage and large-app offers, page dialogs, file downloads and audio sources show `...evil.example` for `accounts.google.com.<many letters>.evil.example`, so the part that names the site's owner is always on screen. Before, they showed the start and cut the end, so such an address looked like Google's. Page dialogs also name hosts longer than 71 characters instead of leaving them out.
- The address keyboard's bookmark and history suggestions and the Collections rows (Bookmarks, History, Saved) now shorten a long address the same way: the path is cut at its end, and a host too long to fit gives way at its start, so they show `...evil.example` instead of `https://accounts.google.com.aaaa...`.
- An address written as a number, such as `http://134744072/` or `http://0x08080808/`, is treated as the address it stands for. A public one now opens over HTTPS first and cannot reach devices on your home network, as any public site; before, it counted as a local name, loaded over plain HTTP and lifted that protection for its page.
- An absolutely positioned grid child placed on grid lines past the 24 columns Tilefinch lays out now fills the grid's padding box instead of taking a size and position read from unrelated memory.
- Grid lists and card grids with more than 64 rows keep stacking rows; every item from the 65th row on used to be drawn on top of the others in one cell.
- Long Hebrew and Arabic pages are now ordered and shaped all the way down; after the first few paragraphs (about 4,000 letters), words were drawn letter by letter in reading order and Arabic lost its joined forms.
- A picture without width and height attributes inside a floated box, inline block, flex item or table cell now widens that box when it finishes loading, instead of being squeezed into the space measured before it arrived.
- Pages with more than 32 tables, such as long Wikipedia list articles, now size every table's columns to their content; from the 33rd table on, columns were split evenly and stopped lining up with colspans and spacing.
- Long tables lay out faster: lining up a table cell's content in the middle or at the bottom (the default for table rows) now only touches that cell, instead of every box and link on the page for every cell.
- A page that sends JSON or custom headers to another site no longer freezes the screen while the browser first asks that site for permission, and it asks once for as long as the site allows (up to 10 minutes) instead of before every request. A page that waits for such a request to finish (a synchronous XHR) keeps the screen responding and can be stopped.
- Settings and history are saved to the Memory Stick less often and more cheaply: a save that would write what the file already holds is skipped, the save waits (up to 10 seconds) while you are pressing buttons or a page is still loading or drawing, the file is written in one pass instead of being read back, and a failing save (full, missing or write-protected stick) is retried at growing intervals up to once a minute instead of every 0.75 seconds.
- An MP3 file from a server that ignores seeking requests now stops at its end and tells the page it ended, instead of starting over forever as if it were a radio station; a large error page with an audio type is no longer taken for a station.
- An MP3 whose tag claims more bytes than the file holds now ends (or, with no audio at all, fails to open) instead of buffering forever, and a radio station whose stream carries such a tag keeps playing.
- Seeking in an MP3 file from a server that supports seeking now lands, instead of staying on "resolving" while the request restarted every frame, and seeking back to 0:00 right after a seek goes back to the start.
- Scrubbing a video back and forth near one spot no longer feeds the decoder the wrong part of the file (a corrupt or refused picture until the next seek): a window downloaded while you were elsewhere is now kept with its own bytes, and a refused download no longer leaves another download's bytes behind under the old window's name.
- A video file whose sound and picture lie apart in the file (fragmented MP4 as ffmpeg writes it, or loosely interleaved MP4) downloads each part of the file about once; before, it downloaded the same 256 KiB again every time playback switched between sound and picture, many times the file's size, and kept pausing to buffer.
- Pressing Play on a page carrying a large data script written to defeat the media scan no longer freezes the browser for a minute or more while it looks for the video.
- HLS video (streams and live channels) starts sooner: while opening, the browser now reads up to 4 KiB of the first segment per screen frame instead of one 188-byte packet, so a large first picture no longer adds seconds to the start.
- A live stream whose sound comes as a separate track now starts its sound in step with the picture. Before, the sound could play seconds ahead of or behind the picture when the channel published a new segment while the browser was opening it, when the sound and picture tracks number their segments differently, or when their segments have different lengths, including a sound track whose segments lack the timestamp the HLS standard requires (it is now placed by its position in the playlist).
- A live stream whose sound comes as a separate track now stays in step after the browser falls behind and jumps ahead to the live window. Before, the sound and picture each jumped by their own amount, so when only one of them jumped, or they jumped by different amounts (which happened even when both had fallen out of the window, since their playlists refresh at different moments), they stayed apart by the difference for the rest of the session. The track that did not jump now leaves out the same stretch and no longer downloads its segments. A jump whose length nothing measures (the track was renumbered and its clock restarted) is left to the track that jumped, as before.
- An HLS stream with sound only now says "No supported codec" at once, instead of failing with an internal queue error or waiting until the open timed out; a video stream whose first picture comes late in its segment now opens instead of failing the same way.
- HLS video with its sound as a separate track uses 512 KiB less memory, and seeking in HLS video or crossing an ad boundary no longer clears that much memory each time.
- A round-the-clock live channel no longer freezes the rest of a segment on one timestamp when its stream clock wraps around, which happens about once every 26.5 hours.
- Longer HLS videos play: a streamed video may now list 2,048 segments in 128 KiB of playlist, about 68 minutes at 2-second segments or 3.4 hours at 6-second ones (about 400 segments when each has a long signed address). Before, more than 256 segments or 64 KiB of playlist (about 8 to 25 minutes) failed with an internal error. A longer one now says "PSP can't play this video format" with "Video playlist too long" (translated in all 13 interface languages, translations v16) and lets the page's next source play. A live channel that lists more than 256 short segments now plays from its newest ones instead of failing.
- Pages that look up the same elements by id again and again, as many do while starting up and on timers, no longer search the whole page for each lookup; the answer is reused until the page changes in a way that could move an id.
- Adding elements to a page no longer re-checks everything already under the new parent for scripts to run, so pages that build long lists one row at a time spend less time doing it. A script the page left inactive and later changed to a runnable type now stays inactive when something else is added, as in other browsers.
- `getElementsByClassName` and `getElementsByTagName` now return live lists that follow later changes, so a page that removes a class from every match in a loop no longer runs until its script is stopped.
- A page watching an element's attributes with a `MutationObserver` now hears about inline style changes made through `element.style`, `setProperty`, `removeProperty` and `cssText`, as lazy loaders and "became visible" checks expect.
- Pages that read their settings from `data-` attributes, such as Bootstrap 5 menus, modals and tooltips, now see them: `element.dataset` can be listed and copied, and a value removed with `removeAttribute` is gone from `dataset` too.
- Pressing a button that needs the page's scripts after they stopped (for example when the page ran out of memory) now says "PAGE SCRIPTS STOPPED - RELOAD TO RETRY" instead of doing nothing, in all 13 interface languages (interface translations v16). With JavaScript turned off, presses stay silent as before.
- `prompt()` gives the page what it offered or what you typed, spaces and line breaks included, rather than the tidied text the dialog shows.
- Web components keep their styling however many rules match: an outer page's rule beats a component's `:host` and `::slotted()` rules past 128 matches as it does below that, and a component's own `!important` beats the element's inline `!important`.
- A picture you scroll to no longer stays blank when image memory is held by repeated images far away: all copies of such an image now make way together.
- A page can run code inside an iframe when the iframe's own security policy allows it, and can't when it doesn't, whatever the page's policy says. Google's sign-in page needs this for its security check, which always failed before; the page now takes about 15 seconds longer to settle on a PSP while that check runs.
- Internet Archive search shows results again: pages can now compute SHA-1, SHA-384 and SHA-512 hashes, not only SHA-256.
- Pages built from web components, such as Internet Archive search, use less memory (about 3 MiB less on Internet Archive search), so they are less likely to run out of page memory.
- Internet Archive search results look closer to a desktop browser: full-width search box, styled result cards with thumbnails and view counts, the view-style buttons, and tab labels clear of their underline.
- Forms sent with a button or Enter now carry what the page's scripts put in them, including long hidden tokens and `formdata` additions, instead of the original markup values. Sites that check such a token, as sign-in pages do, could turn the submission away.
- Forms with values over 4 KiB are sent, up to 64 KiB. A form that cannot be sent says so instead of doing nothing, in all 13 interface languages (interface translations v16), and a form whose script-filled values are too long is no longer sent with the original markup values.
- Submit buttons' `formaction` and `formmethod` are honored without scripts as well as with them.
- A full cookie jar now retires its oldest cookies rather than refusing new ones, holds 16 KiB of values instead of 4 KiB, and a redirect chain may set 64 cookies instead of 32. A sign-in could otherwise lose its session cookie or fail with a transport-limit message.
- Cookie `Expires` dates in the common dashed form are understood, so a site signing out or replacing a cookie removes the old one.
- A navigation answered with HTTP 204 or 205 stays on the current page instead of blanking it.
- Finishing the on-screen keyboard fires `change` on the field, for pages that enable their next step on it.
- Redirected form submissions no longer send `Origin` on the resulting GET, and `Sec-Fetch-Site` stays `cross-site` once a chain has left the initiator's origin.
- Frame `eval` accepts the same script size as a page script, instead of stopping at 64 KiB.
- A long YouTube result title now ends in an ellipsis instead of stopping mid-word.
- An accent on a capital letter sits above it instead of running into it, with a language pack that includes those pairs.
- Hindi and Bengali syllables a language pack has no shaped form for now draw their leading vowel in front of the consonants.
- Arabic and Hebrew video titles, track labels, chapter and queue names are now ordered and shaped in the player.
- Arabic and Hebrew page titles now read in the right direction in the tab list, on HOME, in the address bar, in bookmarks and history, and in search suggestions; a long one loses its end instead of its beginning.
- A Russian page with stress marks no longer spends a language-pack slot on Extended Latin or offers it.
- Arabic now forms the lam-alef ligature, which the language requires, instead of drawing lam and alef apart.
- A language-pack download that fails from the menu now says why, and failures name the cause (server status, memory, a Memory Stick write) instead of a generic or misleading message.
- Fixed language packs failing to install: the downloaded pack now stays in its own folder on the Memory Stick while it installs, where moving it between folders could fail.
- A newly installed language pack now shows on the page you are on at once, without scrolling or reloading.
- Language-pack text is sharper at small sizes and keeps an even stroke weight at every size.
- Bold text in language-pack scripts now looks bold, including headings next to bold Latin text.
- Fixed language-pack text being measured too narrow on pages that ask for Arial, Helvetica or a system font, which could make it overlap what followed.
- Captions, video titles and track names in non-Latin scripts no longer lose their last words or end in a broken character.
- Captions in a script the page does not use now show once that language pack is installed, and a page re-measures its text when its pack attaches.
- Accents and vowel points from language packs that support it now sit on their letter instead of taking a space of their own.
- Arabic letters now stay joined when vowel marks are written between them.
- Arabic and Hebrew captions now read in the right direction, with joined Arabic letters.
- Pages can show alert, OK/Cancel and text-entry messages; Square stops a page that keeps sending them.
- YouTube's free-with-ads movies and shows now say THIS VIDEO IS COPY-PROTECTED with "PSP can't play DRM videos". YouTube sends them only DRM-protected, which the PSP can't decrypt. Before, the player reported a confusing format error. Full movies that channels upload as ordinary videos still play.
- YouTube rentals and purchases now say "PSP can't play paid videos". Before, they said "YouTube refused it. Try later", which suggested that trying again could help.
- Added MP3 playback for page audio: MP3 files and MP3 internet radio streams play in the native player, including SHOUTcast and Icecast stations and direct links to MP3 files.
- Secure radio directories whose stations stream only over plain HTTP now ask once per site, "PLAY UNENCRYPTED AUDIO?", instead of failing. The answer covers only that site's audio until you exit and can be changed under Site information → Permissions & controls → Unencrypted audio.
- Audio-only playback (page audio and YouTube audio-only) no longer stops after 30 seconds while the player waits for a picture the audio never has.
- Retry, and Play anyway on the unencrypted-audio question, now play when the first attempt failed before playback began, instead of opening the player paused. Audio that never starts reports "AUDIO DIDN'T START" rather than a video error.
- Page audio and internet radio now get a listening screen instead of an empty video player: the station or track name (from the station's own name, the audio's title or the page title), the playing, paused, buffering or reconnecting state, the station's genre and server, and a "Not encrypted" mark for stations allowed over plain HTTP. Radio shows LIVE with the listening time instead of "0:00 / 0:00" and no seek bar or seek hint; files keep their timeline.
- Opening, retrying, reconnecting and failing audio now name audio ("Opening audio", "Reconnecting...", "AUDIO DECODER COULDN'T START") instead of video, and a retry after audio failed to start resumes playing instead of opening paused.
- Fixed plain-HTTP radio streams that redirect, after Play anyway: the redirect was refused with "response redirect target is invalid". The same fix applies to any page that has Mixed HTTP (session) turned on.
- SHOUTcast stations whose address is the server's root, which sends web browsers to a status page, now play from the server's stream address ("/;"). That is the address the server's own playlist gives, and over HTTPS when the server offers it.
- A stream address that does not open now shows "AUDIO SOURCE COULD NOT OPEN" (or "VIDEO SOURCE COULD NOT OPEN") with the server's status, for example "HTTP 302", instead of the raw error text. A probe that got no answer shows "NETWORK PROBLEM".
- The ⓘ information symbol that sites use for "more info" buttons now displays instead of a box.
- `atob` and `btoa` accept any input that fits in page memory; they no longer refuse inputs over 1M and 256K characters.
- Going from one Wikipedia article to another (page JavaScript off, the PSP default there) now reuses the stylesheet already compiled for an earlier article's shared head styles and compiles only the new article's own `<style>` blocks. From the third article of a session the page appears sooner: 7.4 s to 2.7 s in the emulator's reload journey at 111 MHz, with about 0.5 MB less memory at its peak; the time on a PSP is still to be measured.

## 0.1.47 — 2026-11-07

- Fixed opening links whose addresses end in a slash.
- Added Page zoom (50%–150%) and smaller web-page text sizes in Settings.

## 0.1.46 — 2026-11-06

- Reliability updates and clearer error reports when opted-in.

- The validation clock worker retains refused startup/shutdown cleanup, rejects requests after stopping begins, and cannot overwrite an unfinished worker during restart.
- Firmware-video allocation failures now use the existing bounded page-memory recovery and clearer memory guidance, with buffer-level numeric details in opt-in error reports.
- Keep presentation blocked when a new suspend interrupts an older recovery.
- Repeating Save for an active video preserves its current download instead of restarting resolution beneath its pending transfer.
- Aborting a request now resets its response and reports the correct event states without erasing a replacement request started by a handler.
- IndexedDB upgrades wait for closing connections' accepted transactions, resume when a blocked handler closes the last connection, and reject an already-closed upgraded connection instead of reporting success.
- Terminating a worker also aborts its unfinished database writes after it has requested connection close.
- Exiting Wi-Fi sign-in preserves ordinary tabs and their saved page, and background site-data restoration waits for the ordinary session to return.
- Network shutdown retains ownership of modules that refuse to unload so later teardown or restart can handle them correctly.
- Failed returns from Wi-Fi sign-in preserve the saved tab behind Home, and selecting that tab reopens its page instead of doing nothing.
- Stream cancellation now waits for active sink work, rejects queued writes, and avoids duplicate cleanup; failed private-file writes release their locks without changing the saved file.
- Messages and page observers recover when a full timer table frees up, rather than leaving initialization or layout updates permanently stalled.
- Worker messages reserve delivery before transferring ownership, so temporary task pressure reports a retryable refusal instead of silently losing detached data.
- Clearing page timers no longer cancels browser deliveries, animation callbacks of another kind, or another worker's timers; reentrant delay conversion also respects the existing task bound.
- Failed or cancelled language-pack installs restore the surviving glyph packs, while active installs prevent page hints from reopening a generation being replaced.
- Closing game audio freezes its clock without rewinding, and retained sources from a closed context cannot start in a replacement mixer.
- Interface translation downloads now supply the native transport's required pinned-resource policy instead of failing before admission.
- File reads remain retryable under task pressure, honor cancellation between phases, and keep old completion events or handler exceptions from corrupting a replacement read.
- Streamed events discard incomplete data between connections, recover deferred retries, and keep handler failures or Close from corrupting later delivery.
- Removing a font pack restores surviving interface and page fonts even when the filesystem refuses the removal.
- Failed frame setup keeps its memory-failure evidence through cleanup, so the surviving page remains readable with an orderly script shutdown.
- WebSocket failures reliably finish their Close notification, discard late messages after closing begins, and permit replacement connections from Close handlers without losing native ownership.
- Native network events reject incomplete values after allocation refusal instead of entering callbacks with invalid data, and multiplayer channels retain reliable failure and reconnection ownership.
- Multiplayer cancellation and shutdown retain worker memory and networking until retirement succeeds, preserve pending Close events, and prevent timed-out startup from restarting later.
- Multiplayer approval keeps the selected peer stable through handshakes and retries, while bounded command queues preserve capacity for sending and closing under routine command pressure.

## 0.1.45 — 2026-11-02

- Clearer, more compact YouTube pages, with better channel and playlist information, improved account feedback, and updated translations.
- More reliable playback and captions, clearer player controls and errors, and smoother page navigation.

- Oversized JavaScript closures fail safely instead of overflowing their captured-variable indices, and repeated built-in symbol-key coercions no longer retain references.
- Play all and playlist pagination advance through pages of unavailable videos without looping or losing their removable rows.
- Failed caption-window extensions retry with a bounded delay while the current captions remain available.
- Moving between controls with the same label restores the focus hint, and queued input stops background paint preparation between units.
- Long YouTube titles and channel names now show as much as fits, cut between characters. Before, a search card whose title was longer than about 255 bytes (85 Japanese, Chinese or Korean characters) said "YouTube video" instead, and long channel names and video-page titles could end in a broken character.
- Private and deleted videos in a YouTube playlist are shown as dimmed rows with YouTube's own title (such as "[Private video]"), with no picture or player, and are left out of the playlist queue and Play all, which could start on one. On your own playlists they keep their Remove action.
- More YouTube links open the right page: `http://` video and Shorts links, `/live/` and `/embed/` video links, and links ending in a slash (such as `/shorts/ID/`, `/results/`, `/feed/history/` and `/playlist/`) used to open YouTube Home, and a channel tab link ending in a slash (`/@name/videos/`) opened the channel without selecting that tab.
- A YouTube search longer than the search box holds (255 bytes) is cut between characters, and the search box, the heading, the request and the next page all use the same text. Before, the cut could fall inside a character and was sent that way.
- Videos on a YouTube channel's own tabs show their view count and age; when YouTube sent them as a single line, the view count was shown as the channel name and the age was lost.
- When the next page of YouTube search results, a channel tab or a playlist can't be loaded, the page now says "More results could not load." with a Try again link. Before, the first page was shown again in its place, whose Load more led back to the same failure.
- Scheduled YouTube streams and premieres show their start date (from YouTube's start time, in UTC) where YouTube's own page fills it in; when no start time is given, the line is left out instead of showing a placeholder.
- On the YouTube home page, Refresh suggestions sits under the "Suggestions will load in the background" line instead of overlapping the top of the welcome box.
- A YouTube video page, signed out, shows Sign in only once, in the header; the second Sign in beside the channel name is gone.
- Empty YouTube pages use plain wording: "No videos found. Try another search.", "No videos found." on a channel tab and "No comments to show." replace messages about a "bounded public response".
- YouTube captions stay right through long videos. After about 35 to 50 minutes of automatic captions, lines from earlier in the video could show in place of the current one, and on a long video captions could stop for good partway through, or vanish after a rewind to before the point where they were turned on. The player now fetches the next stretch of captions before the current one runs out, and fetches them again after such a rewind.
- When a YouTube video can't play, the player says why and what to try, in your language: for example THIS VIDEO NEEDS SIGN-IN with "Sign in on YouTube, then retry", THIS VIDEO ISN'T AVAILABLE with "It may be private or removed", NETWORK PROBLEM, or NOT ENOUGH MEMORY FOR VIDEO with "Close other tabs, then retry". The technical code stays as a small line in the panel's corner, for bug reports. Before, the panel showed raw codes such as "AV WP 800200D2" or "playability: login-required".
- YouTube's reasons for refusing a video are read more carefully: a reason that merely contained words such as "page" or "message" was reported as an age check, and an ended premiere as a stream that had not started yet.
- A YouTube video that is not live no longer opens as a live stream, with a LIVE label and no seek bar, when YouTube offers it only in a format the PSP can't play plus a streamed version; Tilefinch asks YouTube for another version instead.
- A YouTube video whose links expire too soon now fails with that reason instead of an unrelated one, caption tracks are downloaded only from YouTube's caption address, and a video no longer fails now and then because a number on its page was read before all of it had arrived.
- Square turns captions on and off during playback in one press. Turning them on brings back the track that was last showing, or the first one in your subtitle language; a track picked in the menu becomes the one Square brings back.
- The player's hints name every button they use: the time row ends with LEFT/RIGHT 10S (D-pad seek), Square's CAPTIONS where the video has captions, and O CLOSE; the track menu's footer says L/R switches its tabs; and the YouTube login offer says Select also answers Not now.
- A video that is still opening after 10 seconds says "Still loading — O to cancel". A switch to 240p, chosen on the failure panel or made by the player on its own, says "Retrying at 240p" until the video plays or fails.
- Once the bars have hidden themselves, moving focus with the D-pad to another labelled element, such as one of a YouTube video page's action icons, brings back the bottom hint bar naming it until it hides again.
- Signing out of YouTube also signs out the account used to sign in, so the next Sign in asks which account to use instead of silently signing the same one back in.
- When YouTube no longer accepts a saved login, Tilefinch removes it and says SIGNED OUT BY YOUTUBE. Before, the dead login was restored on every start.
- Like and Unlike work on YouTube video pages opened from `/live/` and `/embed/` links; before, they failed without asking YouTube.
- The new player messages and hints and the new YouTube page messages are translated in all 13 interface languages, in a new version of the interface translations (v14). In Bengali and Assamese, YouTube's Filters and Apply filters stay in English for now.
- YouTube's Sign out moved from the page header into the Account panel, and it now asks first: "Sign out of YouTube and your Google account on this PSP?", with Cancel selected. After you sign out, Tilefinch says SIGNED OUT; before, one press signed out with no question and no message.
- Signed out, the YouTube home page is just the header and the search box; the "Watch YouTube on this device" panel shows only while signed in.
- YouTube search results fit more on the first screen: Filters is one short line under the heading, and result titles are held to two lines.
- New setting: **Settings → Video → Channel banners** (Show or Hide, Show by default). Hide leaves the banner off YouTube channel pages without downloading it, which saves several hundred KiB of memory and brings the Home, Videos, Shorts and Playlists tabs onto the first screen. Each shelf's See all link now names its shelf.
- YouTube search results are compact rows by default, including for existing profiles: earlier versions saved Detailed whether or not it was chosen, so this release moves every profile to Compact once. Detailed stays one setting away, and a choice made from now on is kept.
- On a YouTube playlist, the hint bar names the row you are on ("Play from here: Episode 2: ...") instead of only "Play from here".
- The focused YouTube search box has the same white ring as the other YouTube controls, and a video's category is listed after its description instead of on the first screen.
- Correction to 0.1.44: the entry on YouTube channel pages says a signed-out channel page shows only Subscribe. As the later entry in that release says, signed out a channel page shows no Subscribe control; Sign in is in the page header.

## 0.1.44 — 2026-11-01

- Browse YouTube channels with Home, Videos, Shorts and Playlists tabs, clearer live and upcoming stream labels, and more Up next suggestions.
- Play videos from more websites, with better video detection, smoother playback and more reliable recovery after connection drops.

- Streamed (HLS) videos encoded with B-frames play more evenly. Their segments carry no picture durations, so each picture's length was taken from the gap to the previous one in decoding order; for the pictures B-frames are predicted from, that gap spans several frames, so such a picture was treated as due up to about 80 ms early and the already-decoded pictures shown before it were thrown away, leaving pauses of up to 130 ms. Every picture now lasts one frame, measured from the stream's own timestamps.
- Videos the optional software decoder cannot decode at full speed play more smoothly. When it fell behind, the decoder used to skip some pictures on a fixed pattern whether they were late or not, and the pictures it kept then often arrived too late to be shown: one page showed about 12 pictures a second where decoding every picture it could showed about 20. It now skips only pictures that are already late, and otherwise just simplifies the least important ones.
- Offline YouTube downloads and other fragmented MP4 videos played through the optional software decoder no longer stop after a couple of seconds with "advance failed: invalid length-prefixed H.264 AU". The decoder sized its picture and audio buffers from the largest pieces of the video it could see when it opened, but in a fragmented file (every downloaded DASH video) that is only the first two seconds, and the first larger keyframe after them (twice the size, in a talk video) did not fit. The video buffers now grow when a later piece needs more, up to the player's 512 KiB limit, so ordinary videos keep their small buffers; the sound's buffers are sized for the largest frame AAC allows (about 50 KB more in all), since a later sound frame was also larger than any seen at open. If memory for a larger buffer runs out, the video stops with a memory message instead.
- Videos played through the optional software decoder take less of the main processor and leave more memory to the page. When memory the Media Engine can reach ran out, the decoder's picture ring took further slots from memory it cannot reach, and every picture landing there was converted and scaled on the main processor instead (about half the pictures of a talk video). The ring now stops at the reachable slots when it already has eight, which is still enough to keep ahead of the clock, and hands the rest of that memory (about 2.5 to 3.3 MB) back to the page. On that shorter ring the decoder lowers picture quality only when the ring is nearly empty, not after a brief pause in feeding it, so a talk video plays at full quality throughout.
- Streamed (HLS) page videos that are not live, such as a talk site's videos, can now be seeked: the player shows their length and the seek bar, Right/Left and Cross move through the video instead of only pausing it, and a seek lands at the start of the streamed segment holding the target with picture and sound resuming together, including when the sound is a separate stream. A playlist that declares itself VOD counts as finished even without its end marker. Live streams keep their LIVE label and stay unseekable.
- The browser no longer keeps redrawing a page nobody can see. A per-frame flag was never reset, so after some frames the page was re-rendered on every frame from then on: behind a playing video (taking CPU from the decoder and, on one page, causing rebuffering) and after the video closed.
- Streamed (HLS) videos whose sound and picture arrive in one stream no longer freeze after the first few pictures on the PSP's own decoder. When the stream carried a picture ahead of the sound it belongs with, the picture waited for a free display slot, the sound behind it waited for the picture, and the clock that frees the slot waited for the sound: one page played 14 pictures in 45 seconds. Sound already downloaded now passes a waiting picture, up to the usual half-second lead, so the clock keeps moving; ordinary MP4 videos get the same treatment, keeping their sound supplied while a picture waits. Seeking into the middle of a streamed segment also no longer stops after a few pictures with no sound: the sound from before the seek point is now skipped instead of being offered again and again.
- The optional software decoder's early colour conversion on the Media Engine now hands each picture back to the main processor properly: before a picture is shown, or its slot is reused for the next one, the browser waits for any conversion step the Media Engine has under way to finish (giving up after 20 ms and finishing on the main processor). Before, such a late step could in principle write an earlier picture's rows into a slot after it was shown.
- Videos played through the optional software decoder no longer skip their first seconds: while the player was still gathering its first pictures it counted them as late and skipped ahead to the next keyframe again and again (a 25 fps talk video lost 174 pictures and played at about 19 fps). Lateness now counts only once a picture is on screen and the sound has started, and the talk video plays at 24.5 fps with nothing skipped. Pictures that need no reordering are also converted for display on the Media Engine while they are decoded, leaving the main processor more time.
- Seeking in a video played through the optional software decoder after its sound was given up no longer leaves the picture frozen: the seek gives the sound a fresh start (twice per video at most), and once it stays given up the silent clock restarts at the new position instead of counting up from the start of the video.
- The optional software video decoder now also plays Baseline and Main streams that use CAVLC, such as a talk site's 240p video whose five reference frames the PSP's own decoder refuses. Rebuild the add-on with the updated FFmpeg patch to get it. Interlaced, 4:2:2/4:4:4 and 10-bit streams are still refused before the decoder is loaded.
- Videos played through the optional software decoder have sound again: its audio decoder was given too little memory and every frame failed silently. Audio that keeps failing to decode is now reported and given up after about two seconds while the video plays on, instead of passing as silence; the decoder is restarted after a short run of bad frames. Memory the Media Engine works on is always placed where it can reach it, and the picture slots are no longer charged to the page twice.
- The optional software video decoder now starts with the official browser and plays next to large pages. The browser checks for the add-on at startup and, only when it is installed, keeps 2 MiB more memory free for it, so it no longer fails with OPTIONAL VIDEO DECODER NEEDS REBUILD when nothing needs rebuilding. Its decoder now sizes its picture memory for the reference frames the video declares (about 2.5 MB less for a typical 240p stream) instead of the maximum of 16, and loading it no longer counts 5 MB against the page. When a page still holds too much memory for the video, the browser first drops what it can rebuild and then stops the page's scripts, keeping the page, and says PAGE SCRIPTS STOPPED: NOT ENOUGH MEMORY when the player closes. A decoder the PSP could not start, for example for lack of memory, now shows the PSP's error code instead of asking for a rebuild.
- YouTube channel pages no longer show Subscribe when signed out. Signed in, subscribing is the watch page's bell, beside the channel name: a lit bell unsubscribes when the page says you are subscribed, and when it doesn't say, the bell subscribes and Unsubscribe is in the menu beside it. The description line under the name now opens the channel's full description, with its links (up to 8), view count and join date, read from the channel's About panel in one small request; Back to channel returns.
- Cross on a page's own video now plays it even while a page-scripts notice is showing, as it already did for the Play card, and also with focus on the play button of that video's player. A video element with focus opens in the PSP player even if the page's script ignores or blocks the press.
- The page the browser opens at startup now gets the same checks as any page opened later: its declared video's Play card, blank-page recovery and automatic Reader. Before, a page opened at startup got its Play card only after its scripts were stopped.
- The Play card for a page's declared video now appears within about 3 seconds of the page loading even when the page never goes quiet (timers or requests that keep running); before, it could wait until the page's scripts stopped. With focus on the card, Cross plays the video even while a page-scripts notice is showing, instead of stopping the page's scripts.
- Playing a page's video through its player page works more reliably: pressing Play no longer brings up a "page scripts stopped" recovery screen for the page being left, which covered the player page and kept it from starting its video, and a player page now gets 40 seconds instead of 20 for the scripts that run while it loads, since one player took about 20 seconds on a PSP and sometimes lost all its scripts at the limit.
- YouTube search results, channel pages and feeds mark live streams with a red LIVE label in place of the length, with the number watching, and mark scheduled streams and premieres with YouTube's own Upcoming label and their start time. Pressing a scheduled stream's card opens its page instead of trying to play a stream that has not started, and a live stream's page no longer shows a length of 0:00.
- An iframe's picture no longer vanishes from its page when a background image load that was still running is abandoned, for example because the page is laid out again. The same fix stops the host media lab's YouTube player from losing its video surface to such a load, which showed MEDIA IS DETACHED FROM THE PAGE over garbled stripes.
- Playing a page's video through its player page no longer fails on large player pages when memory is tight: the downloaded page is now held in a buffer that grows a quarter at a time instead of doubling, so a 1.1 MB player page needs about 1.25 MB instead of 2 MB before the page being left is released. Before, such a page could end on the failure screen with "stream consumer rejected response body".
- YouTube pages keep working when YouTube sends their page data in its newer form, inside a `<script type="application/json">` element, instead of as a script variable: search, channels, playlists, Explore topics and watch pages all read the data in either form, and in the object form desktop pages use. Before, a page sent this way came out empty.
- Portrait page videos in the PSP's basic H.264 format up to 272x480 (for example a 202x360 news clip) now play instead of failing with "profile/level unsupported", and when the PSP's decoder turns a page video down for its format, level or picture size, Tilefinch now tries the page's other versions of the video (including its streamed version) before giving up, using the optional software decoder first for small pictures encoded above level 3.0. When none plays, the message names the last version's reason.
- The Play card for a page's declared video now also appears when some of the page's script files failed to load but its application keeps running: such a page's empty video player no longer hides the card, and the card is tried again if memory was short the first time. The browser log now says whether the card was shown and why not (`tilefinch-media-card:`).
- A page that declares its video but whose own player can't play on the PSP (for example one that streams through Media Source, or never gives its video element a source) now offers a Play card at the top of the page, as pages whose scripts stopped already did, and the first d-pad press goes to that card. Play opens the page's declared stream, or its player page when that is all it names. Pages with a video the PSP can open keep their own player; pages that declare no video are not checked or delayed, and on a page short of memory the card is shown without its picture.
- YouTube video pages list up to 12 suggestions under Up next (was 6), leaving out ads, Shorts shelves and mixes and showing suggested playlists as playlist cards; the channel name, with its picture, now leads the video's action row and opens the channel. Signed-in accounts get a History page (Account → History, and beside Watch Later and Playlists) with resume positions and Load more; signed out, it points to Sign in.
- Pressing Play on a page's video now stops the page's own downloads while the player is open, so they no longer compete with the video on slow Wi-Fi. Script files the page was still loading wait and load after the player closes, and the page's other new requests wait too; before, such downloads kept running and failed about a minute later, which could break the page.
- YouTube channel pages show the channel: its banner, avatar, name, subscriber count and a line of its description at the top, then the shelves the channel arranges on its Home tab, in its order and under their own headings (up to 8 shelves of 4 entries each, with See all where the shelf has one). Home, Videos, Shorts and Playlists tabs sit under the header; Videos lists the latest uploads with More videos for the next page. Playlists appear as playlist cards with their video count and open the playlist, and live and upcoming streams are labelled LIVE or Upcoming with their watching count or scheduled time. Signed out, the page shows only Subscribe, which leads to sign-in; signed in, it shows only the action that applies when the page states whether you are subscribed. Before, a channel showed a flat list of its Home tab's first entries under "Channel videos", including playlists as if they were videos, and both Subscribe and Unsubscribe even when signed out.
- Streamed videos start on slow Wi-Fi: a video playlist or segment request now has 8 seconds instead of 3 to look up its server and connect, since name lookups alone took over 2 seconds on a PSP.
- Large pages have more memory for their scripts: the text of a page script of 8 KB or more is no longer kept inside the JavaScript heap but in a separate store that compresses it when memory gets tight, which saves about 1 to 6 MB of JavaScript memory on video and news sites. Functions still return their exact source from `toString()`, and error positions are unchanged. A page that reaches its memory limit now reports that once instead of dozens of times, as waiting promises no longer all retry the same function that could not be prepared.
- Pages that read large JSON data need less memory: `JSON.parse` no longer makes a temporary copy of its text, which took up to three times the text's size and was where some video pages ran out of memory.
- Page videos play in more cases: a stream address with no file extension whose server sends the playlist as plain text or as a generic file is now recognized as a live stream even when the server answers only part of the file, a cookie such a stream sets on its first answer is kept for the requests that follow, and a video declared in the body of a page is remembered before the page's own scripts remove it.
- A page that runs out of memory while it is laid out again from inside an iframe's load or error handler, or while its scroll position is put back on Back, now stays on screen as a static page with JavaScript stopped. Before, the browser could read memory the stopped page had already released (and crash), or report "JavaScript scheduler tick failed".
- A page that is running out of memory gets more room for its scripts: the text of script files it has already compiled, kept for a later visit or still held by the script that is running, is let go when the page needs the memory, instead of the page stopping with an out-of-memory error. On large video pages this is up to about 0.75 MB; captured video pages ran into fewer out-of-memory errors. A later visit to such a page fetches those script files again, but still skips compiling them.
- A page whose update runs out of memory while it is being laid out again, for example after its scripts change it, now stays on screen as a static page with JavaScript stopped, as for any other out-of-memory page, instead of disappearing with "page update failed". If even the static page no longer fits, Tilefinch now says the page ran out of memory, in your language, and offers to reload it in Basic view.
- Playing a live channel that is offline now says STREAM OFFLINE OR NOT AVAILABLE (translated) instead of a video error, and when Play went through the channel's player page it goes back to the page, as when nothing playable turns up. A stream address that answers 404 or 410, an empty or ended playlist, or an error page gets this message; a video the PSP can't decode keeps its own message. A video address without a file extension is now opened as a stream when it answers with a playlist, and as a file only when it is one.
- A page's video stays playable when the page's own scripts remove its video details from the page after it loads, as some news sites do while starting up: Play still offers the video, or the player page, that the page was sent with. When the page moves to another article without reloading, or changes its address, the earlier video is no longer offered.
- When a page has no video Tilefinch can play but names its own player page (for example in its `og:video`, `twitter:player` or schema.org `embedUrl`), Play opens that player page and plays the stream it requests, then Back returns to the page. The player page loads on its own, without ads, analytics, consent or chat scripts from other sites, and stream addresses are read from its JSON and playlist answers, including other sites' answers it may read, ignoring related videos, ads and error clips. If nothing playable turns up, Tilefinch says so and goes back.
- Pages using browser-capability checks no longer wipe themselves when a loader checks that `DOMTokenList`, `childElementCount` and members such as `Node.prototype.textContent`, `Element.prototype.innerHTML` and `HTMLIFrameElement.prototype.contentWindow` exist where browsers define them, and these now do. Pages can create `StorageEvent`, `HashChangeEvent` and `PopStateEvent`, and same-document navigation fires the last two with their `oldURL`, `newURL` and `state`. Built-in methods no longer carry a `prototype` property or work as constructors, and `URL`, `URLSearchParams` and `IntersectionObserver` report their own names.
- A page that keeps one request open, such as a live-update stream, no longer holds up its other `fetch()` and `XMLHttpRequest` requests until that request closes. Cookie notices and video workers no longer wait behind a page's feature-flag stream.
- Playing a video page's video no longer starts another clip from the page's list of related videos when that list loads after the page, or when the page's own video is listed only among them; the page's own video is chosen even when a related clip is listed first or in a larger size. When the page's own video comes both as a 720p-or-larger file the PSP can't decode and as a stream, the stream is tried first.
- Scripts running from a `blob:` worker can load scripts and data from full web addresses; video workers no longer stop at `importScripts`. Pages can decode UTF-16 text with `TextDecoder('utf-16le')` and `TextDecoder('utf-16be')`.
- Playing a page's video no longer starts an ad, a promotion or an app-download clip listed in the page's data instead of the page's own video; if the page's own video can't be identified, nothing is offered. A video whose address has no file extension is found when the page's data names it as the page's own video, and so is one listed deep inside a large page-state script.
- With the optional H.264 High decoder installed, videos up to 432x240 that the PSP's own decoder can't play, including High-profile and HLS streams, play at full speed with sound. After a seek, playback resumes from the nearest keyframe, so sound comes back within about half a second. Videos whose sound is HE-AAC or uses 960-sample AAC frames show a message instead of playing with broken sound.
- Page videos the PSP's decoder can't handle, such as a B-frame pyramid or more than 3 reference frames, are recognised before playback starts. Tilefinch tries the page's other versions first and, if none fits, the failed-video panel says why in your language, including when the optional decoder would play it.
- When a page offers several qualities or `<source>` versions of a video, Tilefinch picks one the PSP can decode and moves on to the next if the decoder refuses it. Starting playback picks the page's own video rather than a related preview, and also finds videos whose address appears only in the page's player script or in a quality list the page loaded.
- Page videos and HLS streams that redirect to another site now play. Every playlist, quality level and segment of a page's video, including redirects, follows the page's `media-src` Content Security Policy, the content blocker, the `https` upgrade and the block on local-network addresses.
- Pressing a link that also opens a pop-up behind the page now follows the link, instead of sending the page to the pop-up's site.
- A page that runs out of JavaScript memory and is left blank switches to Basic view instead of staying blank.
- A page whose JavaScript runs out of memory while setting up an embedded frame now opens without that frame, with JavaScript stopped as for any other out-of-memory page, instead of failing to load. The same holds when the frame finishes loading, or the page's handler for its `load` or `error` event runs out of memory: the page keeps working instead of stopping with an error.
- Basic view no longer freezes on a page whose scripts failed to start. Tilefinch freed part of the page twice when it showed the page's fallback content, which could corrupt memory. Affected pages now open in Basic view.
- The D-pad moves focus into links and buttons inside scrolling panels within a page, scrolling the panel to show them.
- Inline event handlers such as `onclick` and `onload` can use the element's, its form's and the document's properties by name, as browsers allow. Stylesheets that switch themselves on with `onload="if(media!='all')media='all'"` now apply, and a form control's handler can read its sibling fields by name, its form's `action` and `target`. A bare `remove()` or `append()` in a handler no longer acts on the element or the document.
- Links with very long addresses can be focused. Stylesheets in a page's markup fire their `onload`, and `link.media` reflects its attribute.
- Pages that check for `SharedWorker` before choosing which scripts to load now get their modern version instead of a heavier fallback for old browsers; affected search results now appear. Creating a `SharedWorker` throws a `NotSupportedError` that pages can catch.
- Fully clipped form controls no longer leak their labels into nearby content; `clip-path` also applies to an element's own background and native control ink without changing its layout geometry. The clip is measured from the border box, so a bordered avatar or button with `clip-path: inset(0)` or `circle(50%)` keeps its border.

## 0.1.43 — 2026-10-26

- Moving focus with the D-pad is faster on pages whose focus styles change layout or colour: on a long form, each move took about half as long on the PSP. Only the screens you can see are laid out at once; the rest finish in the background.
- On pages whose focus style changes only an outline, moving focus no longer lays the page out again: on a PSP-3000 a move took 250 ms instead of 650 ms on a long article and 150 ms instead of 1.8 s on a news front page. Those pages now show the site's own focus outline without Tilefinch's focus ring around it.
- A link or button whose focus style changes its border, shadow or `::before`/`::after` content no longer keeps its focused look after focus moves away when the page later changes, and a focus outline no longer stays drawn on the previously focused element.
- An SVG image nested deeper than Tilefinch can draw is skipped quietly instead of reporting an `error` to the page.
- Password fields always show asterisks, even when a page lays them out as blocks or flex items, and an empty password field shows its placeholder. Submit, reset and file buttons keep their label in every layout, an `<input type=submit value="">` no longer draws SUBMIT over its icon, and file inputs always read CHOOSE FILE.
- Resizable textareas show their resize grip, an unsized `<canvas>` in a flex or grid item gets its default 300x150 size, and a block-level `<audio controls>` is 300px wide instead of stretching across the line.
- Reloading or restoring a tab on an old start-page, bookmarks or history address opens the right Tilefinch screen instead of a failed load, and a tab restored onto the screenshot list or the offline library reopens it.
- Grids with `grid-auto-rows` size every row, and `repeat(auto-fill, minmax(25%, 1fr))` and similar percentage floors give the right number of columns.
- Modern CSS written only in a `style` attribute or set by a script while loading now applies, including `user-select`, `translate`, `rotate`, `backdrop-filter`, `text-wrap` and their `-webkit-` spellings.
- If YouTube topics, YouTube results layout, Website fonts or Night mode can't be changed, the menu now shows the setting still in effect.
- Scroll snapping and `cursor` set inline or by script now apply on every page, `:has()` rules with escaped or upper-case names restyle correctly, and going Back or Forward to native pages stops leftover background work first.
- Unicode regular expressions return safely when memory is exhausted, truncated cached BigInt values release their allocations, and cookie-domain checks use an updated Public Suffix List.

## 0.1.42 — 2026-10-22

- When memory runs out, pages no longer lose click handlers and other event listeners on removed or re-inserted elements.
- Site information, Site data & storage, Help details and the video error panel now appear in your interface language.
- Many more messages appear in your interface language: setting changes, site storage, themes, video errors and progress, the Update and Previous versions screens, and the font-pack install box.
- Interface language downloads are smaller and use less memory; installing one removes its older versions from the Memory Stick.
- After an update, if your interface language or its font pack needs a new download, a notice offers to take you to Interface language.
- Faster D-pad focus on revisited pages and pages with many style tags, faster container queries, and cheaper DOM updates while images load.
- Fixed a possible crash with `::before`/`::after` items in baseline-aligned flex rows; grids using `minmax(min(100%, 20rem), 1fr)` and similar keep their columns; pages with many repeated `@container` rules apply all of them.
- Re-downloading an interface language now works when an older copy is on the Memory Stick, and an unusable saved YouTube login is removed instead of warning on every start.
- `element.animate()` now eases linearly by default, long `linear()` easings work in transitions, section-paged documents keep listeners on more than 128 elements, and an undecodable JPEG now fires `error`.
- Pages with more than 128 elements that have event listeners no longer lose some listeners over time; images that fail to load now fire `error`, including `src=""`.
- Malformed CSS is now ignored as in other browsers: five-value `margin`, invalid transition and animation timing functions, bad `minmax()` and over-long `grid-auto-rows` lists; `env()` fallbacks now apply in more properties.
- `outline` and `border-radius` values containing functions such as `rgb(0 0 0 / 50%)` or `calc()` now apply, as do `outline: thick` and border keywords in any letter case.
- Container queries on `orientation` and `aspect-ratio` now apply, and `@media` accepts range syntax for `aspect-ratio`.
- Pages no longer leak removed elements whose event handlers refer to themselves, and `img.complete` is correct for images that failed to load.
- A signed-in YouTube session with many cookies no longer appears signed out: oversized cookie headers now keep the sign-in cookies instead of sending none.
- Pages that preload many images no longer hang waiting for them, and a new image can no longer show a freed image's size.
- `@container not (...)` rules and long `var()` values now apply; component styles stay inside their component; flex baselines line up with relatively positioned text.

- CSS transitions between named colours now animate; `@container` queries accept `calc()` and all length units; flex baseline alignment honours line-height.
- Pages with very many web components, long `calc()` letter-spacing, or `:has(a:any-link)` rules now style correctly, and an extreme `hsl()` hue no longer hangs the page.
- Clearer download errors, no false 4 GB/s speeds after a server restart, and a translatable Downloads page.
- A `boot.cfg` naming a retired experimental video decoder now starts normally.
- Debug and experiment code removed from the shipped build, which is about 12 KB smaller.

- Variable web fonts, uppercase CSS selectors and web-component styles now render correctly, and SVG artwork past its clip limits appears instead of vanishing.
- Image-heavy pages and canvas games load images reliably: `load` events fire for every image, and drawing an image that is still loading no longer stalls it.
- YouTube links with a start time such as `t=1m30s` open at that time; playlists keep auto-advancing and report a video that can't open.
- Downloads that finished but were interrupted now complete on Retry, and the saved compiled-script cache survives Memory Stick errors.
- Translated interface messages display in full, accented capitals look right, and interface and page language packs load together.
- "YouTube login: Always" in Settings now works.
- Faster styling, SVG drawing, sprite icons and Basic view on complex pages, with less memory per request.

## 0.1.41 — 2026-10-18

- Faster startup, including the first launch after installation. The full startup improvement requires the updated launcher from the install ZIP; in-app updates remain compatible.
- Smoother video playback and faster YouTube loading and search processing, with fewer startup audio interruptions, fewer player-menu hitches, and cleaner loading feedback.

## 0.1.40 — 2026-10-16

- Download files directly to the Memory Stick with pause, resume, retry, and cancellation; preview and install compatible homebrew ZIPs, and manage downloads and installed apps separately.
- More responsive browsing, improved page rendering and Basic/Reader views, and reduced memory use with better low-memory recovery.

## 0.1.39 — 2026-10-15

- More responsive focus navigation and faster loading on complex pages.
- Improved page layouts, text spacing, images, icons, and visual effects.
- Better Basic and Reader views, with more complete long articles and preserved links.
- Improved styling and reliability when memory is limited.

## 0.1.38 — 2026-10-14

- More reliable YouTube sign-in, with clearer recovery and optional privacy-safe diagnostics.
- Faster player menus and smoother page updates.
- Improved memory handling and stability on script-heavy pages.

## 0.1.37 — 2026-10-14

- Playlist playback with Next/Previous, an optional Auto next toggle, and a queue.
- More responsive YouTube navigation and player menus, with clearer controls
  and improved playback recovery.
- Expanded interface language support.
- Safer handling of interrupted HTTP/2 uploads.

## 0.1.36 — 2026-10-13

- Expanded interface language support.
- Clearer guidance for certificate errors and correcting the PSP’s date and time.

## 0.1.35 — 2026-10-13

- More responsive scrolling, focus navigation, and browser controls while pages load.
- Improved page layouts, images, icons, and text, with new website font options.
- Clearer YouTube search filters and compact video actions.
- Improved low-memory handling and stability.

## 0.1.34 — 2026-10-11

- Chapter markers and a chapter picker in the video player.
- More responsive seeking and more reliable playback recovery.
- Clearer translated player controls and smarter rotation options.
- Preserve search filters, scroll, and focus when returning from a video.

## 0.1.33 — 2026-10-10

- Watch Later, playlists, and channel Subscribe/Unsubscribe controls in the
  lightweight YouTube provider.
- Compact search filters and selectable video chapters.
- More reliable account-action confirmations and navigation.
- Updated translations and clearer provider controls.

## 0.1.32 — 2026-10-10

- Sign in to YouTube for recommendations and subscriptions, with optional
  saved login and Sign out.
- Like and unlike videos, and see available watch progress.
- Translated YouTube controls and smoother background updates that preserve
  your place.
- Improved page layouts, text, icons, images, and browsing reliability.

## 0.1.31 — 2026-10-08

- More accurate page layouts, images, icons, and dynamically updated styling.
- Clearer Basic and Reader views, with better article selection, tables,
  navigation, and image handling.
- Preview images for supported video cards that would otherwise appear blank.
- Reduced memory use for page styling.
- Improved ad blocking and additional reliability fixes.

## 0.1.30 — 2026-10-07

Reissued with signed update sequence 31. The public version remains 0.1.30;
existing sequence-30 installations can update normally.

### Interface and browsing

- Downloadable interface languages are available in Settings → Appearance →
  Language & emoji. Install the selected translation and any required glyph
  pack, then restart to apply it. This translates browser menus, not websites.
- Menu-navigation instructions are translated too, with Up/Down guidance and
  bounded fitting for longer labels. Downloads are hosted only by the optional
  resource repository; browser builds retain versioned resource keys and hashes.
- Reader mode reuses stylesheets already loaded by the page instead of
  downloading them again when switching views.
- Cursor, focus and browser controls retain priority while page work is
  ongoing. Video journeys also preserve bounded, in-memory navigation state.
- The Square button shortcut now uses a built-in □ symbol.

### Video and connection support

- Added portrait playback, rotation and center-crop controls, including
  graphics-engine presentation for supported 240p/360p video. Seeking and
  playback controls follow the rotated view.
- The YouTube homepage can load optional topic categories after displaying
  its search box. Disable them in Settings → Video → YouTube home topics.
  Additional video-result card formats are supported.
- Optional verbose Wi-Fi diagnostics provide a shareable on-screen report
  without writing a log for each connection or recording network passwords.

### Memory and reliability

- Reduced WebP decoder scratch memory while preserving exact image output.
- Reduced unnecessary selector-cache allocation and near-limit JavaScript
  collections during array growth.
- Fixed allocation-failure handling for JavaScript bound functions and
  storage ownership when cloning document fragments.

## 0.1.29 — 2026-10-07

### Before you update

- **This version edits your `boot.cfg` once.** The largest single script
  Tilefinch accepts is now 4096 KiB (it was 512). If your `boot.cfg`,
  `data/boot-overrides.cfg` or an old `boot-live.cfg` still says exactly
  `file_kb=512`, the first start rewrites that line to `file_kb=4096` and
  adds a `#file_kb-migrated-from=512` comment, so it never does so again.
  Any other value you chose, including 512 set again after that comment,
  is left alone, as is a `boot.cfg` written by `scripts/stage-psp-game.sh`
  for a staged game, which keeps its 512 KiB Game Profile limit (see
  [Troubleshooting](TROUBLESHOOTING.md#a-site-misbehaves-because-of-its-scripts)).

### Large sites and memory

- Scripts are admitted by the memory their compile needs instead of a fixed
  size cap. A single script may now be up to 4 MB (was 512 KB) and a page's
  scripts up to 16 MB in total (was 2 MB), so the app bundles of many large
  sites run instead of being refused. A script that does not fit fails on
  its own; the page's other scripts keep running. Scripts inside frames get
  the same allowance.
- Stylesheets are admitted by what they cost once parsed rather than by
  three times their size. A sheet larger than the 768 KB per-file limit may
  grow to 2 MB when memory allows, and otherwise is applied up to its last
  complete rule instead of being thrown away, so such pages are no longer
  left unstyled.
- Pages up to 8 MB can be opened (was 4 MB).
- **Heavy pages.** Tilefinch now estimates how long a page's scripts will
  take to start. When a page that is only an empty app shell would need
  15 seconds or more, it asks first, with the size and an estimate: X
  starts it, Triangle always starts it on that site, O leaves the page as
  the server sent it. A page that already shows its text keeps loading in
  the background and says what it costs, and X stops its scripts. An app
  too large for the PSP even in the best case is not started; Tilefinch
  says how much memory it would need and offers Basic view, Reader mode or
  turning off the site's JavaScript. **Settings → Browsing & input → Heavy
  pages** chooses Ask (the default), Run or Basic view.
- If a page's scripts run out of memory and wipe the page as they fail,
  Tilefinch puts back the page the server sent and says "Page scripts
  stopped: not enough memory".
- **Basic view fallback** (**Settings → Browsing & input**) chooses what
  happens when a page's scripts fail or stop and leave it blank or
  unusable: Automatic (the default, as before) switches to Basic view, Ask
  offers **Show Basic view**, Off never switches. When a page failed to
  load after its document arrived, the recovery sheet can also **Reload in
  Basic view** with JavaScript off for that one load. Basic view now works
  on very large pages (it shows a shortened view instead of refusing), and
  Basic view and Reader mode show more of a page when memory allows.
- Images are decoded at the size the page shows them instead of at full
  size and then shrunk, and after layout, pictures and SVG icons that are
  drawn smaller than they were decoded are reduced to their painted size.
  On one image-heavy test page the peak memory fell from 29.6 MB to
  21.0 MB.

### Faster

- Revisited script-heavy pages compile far less. Compiled scripts now have
  their own 3 MB cache; in 0.1.28 they shared the 640 KB page cache with
  images and stylesheets and were almost always pushed out before a
  revisit. Compiled scripts are stored while the page is idle, not during
  the load, and the cache gives its memory back before a page runs short.
- **Keep compiled scripts** (**Settings → Device & storage → Site data &
  storage**) also keeps compiled scripts on the Memory Stick (at most 8 MB),
  so pages you visited before restarting Tilefinch start faster too. It is
  **Off** by default. **Clear compiled scripts** empties it.
- Sites that ship large webpack bundles (such as The Washington Post, XE
  and VK) start faster on the first visit: their bundles' functions are no
  longer all compiled up front just to check their syntax (on The
  Washington Post's home page this was about 8 seconds of PSP time).
  A function with a syntax error now reports it when it first runs. A
  revisit reuses the bundle's factory plan instead of working it out again.
- JavaScript garbage collection no longer thrashes when a page's memory is
  nearly full: in the emulator, one page's longest task spent 25.5 seconds
  collecting and now spends 4.3.
- Pages built with utility CSS frameworks such as Tailwind find their
  images much sooner: in the emulator, XE loads in 80 s instead of 116,
  weather.com in 74 s instead of 131 and Reddit in 26 s instead of 80.
- Large news pages relayout much faster: styles are kept across relayouts,
  container-query pages are laid out in one pass, and changes inside hidden
  content no longer cause a relayout.
- Pages that use only one of the bundled bold fonts load only that one.

### Pages that work better

- Pages that move on with `<meta http-equiv="refresh">` or a `Refresh`
  response header now do. An automatic reload of a page you have started
  using is offered instead ("Page wants to reload", Square reloads), and a
  chain of more than five refreshes without your input is stopped.
  `location.replace()` now replaces the history entry, so Back skips
  redirect pages.
- Documents in legacy single-byte encodings (such as windows-1251 and
  KOI8-R) and in UTF-16 now display correctly, and so do their scripts.
  Chinese, Japanese and Korean legacy encodings are not supported yet.
- Web components' styles stay inside their shadow trees, `:host` and
  `::slotted()` work, and slotted content is laid out where its slot is.
- CSS custom properties may be up to 1 KB long (was 95 bytes), `@property`
  registrations are honoured, and a gradient direction may be combined with
  an interpolation method, so Tailwind 4 gradients and shadows paint.
- Content Security Policy Level 3: nonces, script hashes, `'strict-dynamic'`
  and the separate element and attribute directives are honoured, so sites
  with modern policies (such as Reddit and weather.gov) run their scripts.
- `<noscript>` content shows when JavaScript is off for the page.
- Pages that check for `document.fonts`, `document.all`, printing or Event
  Timing now find them, and permission checks for sensors and other
  features the PSP lacks answer "denied" instead of failing. Also added or
  fixed: `document.links`, `document.anchors`, `document.scrollingElement`,
  rendered `innerText`, `window.length` and `window[i]` for frames,
  `window.origin`, `Range.createContextualFragment()`, `document.domain`
  relaxation, `navigator.sendBeacon()` (bodies were dropped), cross-origin
  `no-cors` fetches, many more `IntersectionObserver`s, and `Intl`'s default
  time zone.
- Layout: wide grids and grid placement, floats and clearfix, `clip-path`,
  sticky bars (no more doubled copies), backgrounds of inline boxes,
  baseline alignment of inline blocks, tables without explicit rows,
  `var()` in inline SVG colours and font stacks, and empty `@media` blocks.
  Page Down no longer scrolls text under bars pinned to the screen edges.
- Pages in Cyrillic and other scripts the built-in fonts lack now offer to
  install the matching language pack, showing its signed size before
  asking again. Installed packs take effect without a restart.
  **Settings → Appearance → Language & emoji → Offer language packs** turns
  the offer off.
- When a site's bot protection blocks Tilefinch, the page now says so
  instead of staying blank.
- **Auto Reader** now recognizes clear articles, decides page by page and
  shows a short note when it switches.
- The search compatibility page can retry regular scripted search with an
  honest PSP identity.

### Installed apps and games

- After a Tilefinch update that changes the compiled-script format, an
  installed app is marked RECOMPILE in **Library → Saved** and X offers to
  recompile it before it opens (or to open it anyway), instead of silently
  compiling every script on each launch.
- Installed apps may precompile up to 1 MB of script (was 512 KB), get a
  larger JavaScript heap (`app_heap_mb` in `boot.cfg`, 9 MB by default),
  keep their script source on the Memory Stick instead of in RAM, and show
  install progress (Circle stops).
- Full-screen WebGL games draw straight through the PSP's graphics chip,
  game audio is scheduled natively, the browser bars are hidden before a
  game's first full-screen frame, and the Page controls notice lasts 5
  seconds. Shader logic the PSP cannot run is reported instead of drawn
  wrongly.
- Treadline Arena gains a campaign (The Line), a Practice Range, daily runs
  and replays, adaptive music, Gunner controls with their own Controls menu,
  an optional aim guide (off by default), larger menu text and a cleaner
  HUD font, with many gameplay and AI fixes.

### Fixes

- JavaScript engine crashes fixed: a function compile that ran out of
  memory could corrupt memory (a crash on GitLab), a regular expression
  compile that ran out of memory could write past its buffer (a crash on
  Mastodon), a page that kept retrying a function the engine had no memory
  to compile could loop until the watchdog stopped it, and running out of
  memory while parsing was reported as a syntax error in the page.
- A paused video rewound to 0:00 resumes again, and the "Opening video"
  notice no longer stays up during playback.
- One stalled script request no longer holds up the page's other scripts,
  and webpack bundles that start with a byte order mark are now split and
  loaded lazily like any other.
- `location` keeps the current URL until a navigation actually commits.
- Pages with more than 8,192 elements no longer exhaust the DOM handle
  table.
- Hardening against hostile pages in the SVG renderer, CSS `calc()` and
  `var()` handling, table layout and the HTML parser.

### Fixes from the pre-release audit

- Pages that call `location.replace()`, or refresh themselves, through a
  built-in site adapter now replace the history entry, so Back skips the
  redirecting page. A `Refresh` header whose address is too long is ignored
  instead of being followed to a cut-off address.
- A site's "always run" or "this session" heavy-page choice applies from its
  first scripts and no longer carries over to the next site. The "Too heavy
  for the PSP" sheet no longer offers Basic view when Basic view fallback is
  Off.
- `querySelectorAll`, `children`, `childNodes`, `getElementsByTagName` and
  `document.all` no longer stop silently at 4,096 results on large pages.
- `fetch()` in `no-cors` mode drops headers it may not send and sends the
  request, as other browsers do, instead of failing (common on analytics
  scripts). `sendBeacon` reports `false` instead of silently dropping a
  beacon when the network queue is full.
- Large lazily loaded webpack modules are no longer refused with "out of
  memory" on first use when the page had room for them.
- "Clear data for this site" also drops that site's compiled scripts that
  were still waiting to be saved, so they are not written back afterwards.
- Inline SVG icons coloured through a CSS variable that holds a modern
  colour (`oklch()`, `color-mix()`, `light-dark()`) paint in that colour
  instead of grey. Long CSS variable values (transition lists, filter
  chains, grid templates and similar) now apply instead of being dropped,
  and `color-mix()` accepts every CSS Color 4 colour space.
- `TextDecoder` decodes every legacy single-byte encoding (windows-125x,
  KOI8, ISO-8859-x, IBM866, Mac and x-user-defined), not only windows-1252.
- `document.all` is much faster in old-style loops and keeps working when a
  page replaces DOM methods; pages without frames no longer walk the whole
  document before every script.
- Constructed stylesheets split their rules the same way `<link>`
  stylesheets do, `document.fonts.check()` uses the real font grammar, and a
  disabled form-associated custom element can no longer be focused.
- Treadline Arena: the boss can no longer clip into a wall or gate corner
  when it shoves or spawns, and menus read the gamepad once per step.
- Treadline Arena: after backing out of Enter code in Multiplayer, the
  Garage's Paint row and the Camera & Display and Audio settings no longer
  disappear, and Multiplayer keeps its heading after you cancel an online
  attempt. The title menus no longer show a gadget bar that never changed;
  the in-game HUD still shows the gadget and Command meters.
- The compiled-script cache kept by **Keep compiled scripts** is discarded
  once after updating, because the engine fingerprint now also covers the
  regular-expression and Unicode tables.
- Debug, test and experiment code (an alternative Web Audio
  implementation, test bridges in Treadline Arena, and leftover
  measurement switches) is no longer part of the shipped build.
- Low-memory style builds preserve ordinary and document-adopted styles
  without letting shadow-confined rules escape their roots. Constructed
  sheets beyond the tracked-source limit are declined safely, and dense
  cascades have an additional bounds check.
- A near-full stylesheet ending in ignored text stays editable, and an
  unreadable compiled-script pack is tried only once per navigation instead
  of repeatedly reading the Memory Stick.
- Refreshed the public suffix data used for cookie and site-boundary checks.

## 0.1.28 — 2026-10-01

- Faster loading and responses on JavaScript-heavy sites.
- More responsive dynamic styling, page updates, and images.
- Improved navigation reliability and memory handling.
- Refreshed security data and fixed a JavaScript engine error-path leak.

## 0.1.27 — 2026-09-23

- Made long pages usable sooner, with more responsive focus, scrolling, and menus while loading continues.
- Improved layout, text, and WebGL rendering on complex pages and games.
- Added per-site storage controls with optional Memory Stick persistence and safer recovery.
- Reduced browsing and playback overhead, with additional stability fixes.

## 0.1.26 — 2026-09-21

- Improved page layout and computed-style compatibility on complex sites.
- Improved PSP cursor responsiveness, UI rendering, and frame scheduling.
- Hardened JavaScript memory handling and runtime diagnostics.

## 0.1.25 — 2026-09-19

- Fixed dynamic styling and motion handling on large pages.
- Improved Worker and origin-private storage performance and memory use.

## 0.1.24 — 2026-09-19

- Improved compatibility for Workers, embedded frames, Web Crypto, storage,
  and other modern web APIs.
- Reduced JavaScript memory use and improved stability on script-heavy pages.
- Fixed additional frame updates, session-data clearing, and platform API
  behavior.

## 0.1.23 — 2026-09-13

- Improved Worker and browser event ordering on complex pages.
- Additional compatibility and stability fixes.

## 0.1.22 — 2026-09-13

- Improved compatibility and reliability for Workers, embedded frames, and
  browser events.
- Updated the PSP network stack with the latest security fixes.

## 0.1.21 — 2026-09-13

- Faster, more memory-efficient web response handling.
- Improved reliability for Fetch, Streams, text decoding, and IndexedDB.

## 0.1.20 — 2026-09-12

- Improved compatibility and reliability for Workers, Fetch, IndexedDB,
  embedded frames, and other dynamic Web APIs.
- Reduced routine Memory Stick writes and fixed additional lifecycle, memory,
  and stability issues.

## 0.1.19 — 2026-09-09

- Improved Wikipedia search forms and results layout.
- Playback-position saving is now off by default, with options to enable it or clear saved positions.

## 0.1.18 — 2026-09-09

- More responsive browsing and improved network reliability.
- Faster video descriptions and comments, with corrected scroll positioning.
- More reliable Wikipedia browsing.
- Weekly automatic update checks enabled by default.
- Additional memory and stability fixes.

## 0.1.17 — 2026-09-08

- Faster, more responsive page loading and navigation.
- Reduced memory use and improved reliability on complex pages.
- Fixed dynamic styling, embedded-frame, and viewport handling.
- Additional playback and stability fixes.

## 0.1.16 — 2026-09-06

- Faster startup and more responsive page loading, scrolling, and focus navigation.
- Improved video loading and playback.
- Improved compatibility and reliability for Workers, WebAssembly, embedded frames,
  and dynamic styles.
- Additional memory and stability fixes.

## 0.1.15 — 2026-09-02

- Added bounded WebAssembly and Web Worker support for more modern sites.
- Improved complex-page reliability with better fallback, Reader, and history
  recovery.
- Added native playback cards for pages that declare playable video.
- Improved JavaScript memory use and hardened browsing, media, and offline-game
  lifecycles.

## 0.1.14 — 2026-08-30

- Added Treadline Arena, an installable 3D tank game with Arcade and Classic
  controls, aim assist, Onslaught mode, and shareable generated arenas.
- Added direct multiplayer support for compatible games, with PSP LAN
  discovery and numeric invites plus service-free pairing in ordinary browsers.
- Improved WebGL and Canvas game speed, input response, frame pacing, audio,
  visual stability, and installed-game startup.
- Improved complex and long-page browsing by preserving useful page content
  when scripts fail and offering contextual recovery through Basic view.
- Made Reader mode more reliable and useful for text, links, images, captions,
  tables, and code, with better position preservation when switching views.

## 0.1.13 — 2026-08-26

- Added selectable Midnight, light, and custom RGB565 themes, with a visual
  theme designer and matching launcher artwork.
- Improved YouTube audio and subtitle switching, caption rendering and style
  controls, and user-friendly track language names.
- Fixed video and subtitle flicker, partial loading frames, and startup A/V
  skew while keeping playback responsive when track menus are open.
- Improved direct YouTube playback and search submission, and expanded the
  included Prism Break 3D game's keyboard and controller support.

## 0.1.12 — 2026-08-24

- YouTube live-stream and premiere support.
- Added audio and subtitle track selection plus preferred, alternate, original,
  and PSP-system language ranking. Captions remain off until selected.
- Added page-control mode and the Gamepad API, expanded Canvas 2D and Web
  Audio support, and started WebGL support. The source tree now includes the
  installable Prism Break 3D game.
- Added installable offline web apps with size previews, update and reinstall
  status, uninstall controls, icons, and bounded manifest presentation.

## 0.1.11 — 2026-08-23

- Added an optional ARK-4 XMB redirect that opens Tilefinch from Sony's
  Internet Browser icon, with an L-trigger bypass and fail-open behavior.
- Added isolated Wi-Fi sign-in for captive portals without weakening normal
  HTTPS verification or sharing ordinary browsing cookies and storage.
- Added YouTube live-stream and premiere support, improved native page audio
  and video routing, and hardened playback, seeking, buffering, and player
  presentation across changing network and media conditions.
- Expanded Canvas 2D for charts, controls, and simple games with native text,
  clipping, paths, sprites, animation batching, shadows, and safer bounded
  raster work.
- Added bounded bidirectional page text, Arabic-family shaping, and optional
  Arabic and Hebrew glyph-pack support while preserving the fast LTR path.
- Improved Reader mode, long-page rendering, script-failure degradation,
  responsive layouts, and static fallbacks for animation-heavy pages.
- Added clearer contextual failure recovery, richer site information, and a
  resumable download manager with bounded Memory Stick writes.
- Improved PSP chrome composition and made HOME-button exit responsive even
  while the browser is cancelling foreground work.

## 0.1.10 — 2026-08-22

- Improved time to playback from YouTube results: Play now opens the native
  player directly and can reuse preparation performed while a result is
  selected; Details remains an ordinary watch page.
- Made YouTube result pages interactive before thumbnails arrive, then load
  and reveal visible thumbnails progressively without stale placeholders.
- Prioritized the focused result's decoded thumbnail before speculative video
  resolution, and let a focus change redirect that work to the new row.
- Made video-resolution progress advance through its real preparation stages
  instead of appearing stuck near the start.
- Fixed confirmed seeks so playback resumes automatically when the video was
  playing, while seeking from Pause continues to preserve the paused state.
- Kept the player timeline stable while a committed seek is loading instead
  of briefly replacing the footer, prevented moving video from bleeding into
  the opaque control strip, and restored one-press Play activation on YouTube
  results when a late relayout invalidates the autofocus region.
- Kept result-page controls responsive while JPEG thumbnails decode and
  arrive, preserving focus and deferred-image order across relayouts.
- Made video opening retry stalled CDN ranges on bounded fresh connections
  before discarding an otherwise valid resolved stream.
- Reduced startup and first-navigation delay by presenting native Home before
  deferred site-data restoration and staging page fonts around network work.
- Expanded HTTPS compatibility and made certificate failures more actionable
  with cause-specific guidance and richer diagnostics.

## 0.1.9 — 2026-08-21

- Fixed the YouTube info control so it opens the watch page without starting
  playback; the result card and watch-page thumbnail remain explicit play
  controls.

## 0.1.8 — 2026-08-21

- Made in-app updates faster while retaining complete package and file
  verification.
- Improved YouTube startup when thumbnails or abandoned page requests are
  still using the shared network worker.
- Improved deferred image loading and recovery after transient failures,
  with less browser-thread work while a page is open.
- Improved playback recovery from transient PSP audio-decoder refusals.

## 0.1.7 — 2026-08-21

- Improved mobile page fidelity across responsive headers, forms, flex and
  grid layouts, inline SVG, layered backgrounds, and script-staged static
  content.
- Added generic structured-audio discovery and native playback for authored
  page audio without requiring full application hydration.
- Improved script compatibility with inline event handlers, cursor hover
  events, correct `nomodule` handling, and soft degradation when optional
  scripts cannot run.
- Hardened flex arithmetic, media-candidate lifetimes, audio-only range
  requests, and media lifecycle transitions under PSP memory limits.
- Added reproducible mobile-viewport capture tools and release checks that
  prevent local build paths from leaking into PSP artifacts.

## 0.1.6 — 2026-08-20

- Improved YouTube playback, seeking, navigation, buffering recovery, and
  thumbnail performance; added audio-only playback and compact results.
- Added generic page-video and bounded HLS support, plus an optional build-time
  software-decoder add-on that remains separate from official releases.
- Replaced site-specific Reader profiles with bounded content analysis and
  added lazy-image sourcing for server-rendered pages.
- Reorganized menus around page tools, library, settings, and diagnostics;
  added signed installation of up to eight recent releases.
- Expanded mobile CSS, typography, compositing, Cyrillic, and Extended Latin
  support while preserving Wikipedia performance and fidelity.
- Hardened native media geometry, AAC channel bounds, script degradation, and
  aggregate backdrop-filter work.

## 0.1.5 — 2026-08-18

- Improved YouTube startup and recovery when the shared transport worker is
  busy or a delivery candidate stalls.
- Added photographed diagnostic reports with exact, bounded QR rendering and
  complete multipart recovery for large logs.

## 0.1.4 — 2026-08-17

- Improved player-control redraw stability during interactive seeking.
- Localized YouTube result and watch metadata from the PSP language and date
  settings without delaying native startup.
- Improved navigation responsiveness by promptly retiring abandoned page
  requests before the next page starts.
- Accepted larger real-world response cookie sets while preserving bounded,
  fail-closed redirect handling.
- Reduced cooperative YouTube parsing work on PSP.

## 0.1.3 — 2026-08-17

- Improved YouTube playback recovery when a delivery URL serves only an
  unusable prefix or rejects later byte ranges.
- Added a bounded 360p-to-240p fallback and more complete last-error details
  for prolonged buffering and terminal media failures.

## 0.1.2 — 2026-08-16

- Expanded the curated PSP TLS trust bundle for more widely used certificate
  authorities and added a native-Mbed-TLS site audit to the release process.

## 0.1.1 — 2026-08-16

- Fixed GitHub certificate verification and expanded device network-error
  diagnostics.
- Added automatic recovery from a missing saved connection and a Wi-Fi
  profile selector under System options, including each saved profile's SSID.

## 0.1.0 — 2026-08-12

- Initial release.
