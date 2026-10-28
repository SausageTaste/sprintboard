export type InitialIndexState = "pending" | "running" | "ready" | "failed";

// Poll only the initial reconciliation. Periodic scans do not replace the
// user's gallery or disturb pagination after startup is complete.
export function watchInitialIndexStatus(onState: (state: InitialIndexState) => void): () => void {
    const controller = new AbortController();
    let timer: ReturnType<typeof setTimeout> | undefined;

    const poll = async () => {
        let state: InitialIndexState = "failed";
        try {
            const response = await fetch("/api/images/index/status", {
                signal: controller.signal,
                cache: "no-store",
            });
            if (!response.ok)
                throw new Error(`Index status request failed: ${response.status}`);
            const data: unknown = await response.json();
            if (typeof data !== "object" || data === null || !("state" in data)
                || !["pending", "running", "ready", "failed"].includes(String(data.state)))
                throw new Error("Invalid index status response");
            state = data.state as InitialIndexState;
        } catch {
            // Connection failures use the same retry interval as scan failures.
        }
        if (controller.signal.aborted)
            return;
        onState(state);
        if (state !== "ready")
            timer = setTimeout(() => { void poll(); }, state === "failed" ? 5000 : 2000);
    };

    void poll();
    return () => {
        controller.abort();
        clearTimeout(timer);
    };
}
