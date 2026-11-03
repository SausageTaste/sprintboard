type RefreshContext = {
    signal: AbortSignal;
    isCurrent: () => boolean;
};

type GalleryPollingOptions = {
    isPaused: () => boolean;
    getRequestGeneration: () => number;
    visit: (signal: AbortSignal) => Promise<unknown>;
    refresh: (context: RefreshContext) => Promise<void>;
    onError: (error: unknown) => void;
};

// Visits keep the selected scope fresh even when no index update is published.
// Their responses are deliberately separate from passive page replacement.
export function watchGalleryUpdates(options: GalleryPollingOptions): () => void {
    const controller = new AbortController();
    let timer: ReturnType<typeof setTimeout> | undefined;
    let lastVisit = performance.now();
    const poll = async () => {
        const generation = options.getRequestGeneration();
        const isCurrent = () => !controller.signal.aborted && !options.isPaused()
            && generation === options.getRequestGeneration();
        try {
            if (!isCurrent())
                return;
            if (performance.now() - lastVisit >= 15_000) {
                await options.visit(controller.signal);
                lastVisit = performance.now();
            }
            if (isCurrent())
                await options.refresh({ signal: controller.signal, isCurrent });
        } catch (error) {
            if (!controller.signal.aborted)
                options.onError(error);
        } finally {
            if (!controller.signal.aborted)
                timer = setTimeout(() => { void poll(); }, 2000);
        }
    };
    timer = setTimeout(() => { void poll(); }, 2000);
    return () => {
        controller.abort();
        clearTimeout(timer);
    };
}
