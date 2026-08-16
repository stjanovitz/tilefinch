(function () {
    "use strict";

    const counts = [0, 0, 0, 0, 0];
    const failures = [];
    const pending = new Set();
    globalThis.pocSummary = "WPT|pending";

    function clean(value) {
        return String(value === undefined || value === null ? "" : value)
            .replace(/[\r\n|]+/g, " ")
            .slice(0, 180);
    }

    const originalAsyncTest = globalThis.async_test;
    globalThis.async_test = function (...args) {
        const value = originalAsyncTest.apply(this, args);
        pending.add(String(value.name));
        globalThis.pocSummary =
            "WPT|pending|d=" + clean([...pending].slice(0, 3).join(";"));
        return value;
    };

    setup({output: false});
    add_result_callback(test => {
        pending.delete(String(test.name));
        if (pending.size) {
            globalThis.pocSummary =
                "WPT|pending|d=" + clean([...pending].slice(0, 3).join(";"));
        }
        if (Number.isInteger(test.status)
            && test.status >= 0 && test.status < counts.length) {
            counts[test.status]++;
        }
        if (test.status !== test.PASS && failures.length < 3) {
            failures.push(
                clean(test.name)
                + ":"
                + clean(test.message)
                + (test.stack ? "@" + clean(test.stack) : "")
            );
        }
    });
    add_completion_callback((tests, harnessStatus) => {
        if (harnessStatus.status !== 0 && failures.length < 3) {
            failures.push(
                "harness:"
                + clean(harnessStatus.message || harnessStatus.stack || "")
            );
        }
        globalThis.pocSummary =
            "WPT|complete|h=" + String(harnessStatus.status)
            + "|p=" + String(counts[0])
            + "|f=" + String(counts[1])
            + "|t=" + String(counts[2])
            + "|n=" + String(counts[3])
            + "|s=" + String(counts[4])
            + "|d=" + failures.join(";");
    });
})();
