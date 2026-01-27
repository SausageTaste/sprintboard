import React from "react";


export default function Gallery() {
    const [idleTime, setIdleTime] = React.useState<number>(0);
    const [wakeStatus, setWakeStatus] = React.useState<boolean>(false);

    const loadMore = React.useCallback(async () => {
        try {
            const res = await fetch(`/api/wake`);
            const data = await res.json();
            setIdleTime(data.idle_time ?? 0);
            setWakeStatus(data.wake_on ?? false);
        } catch (e) {
        }
    }, []);

    const sendWakeUpCall = React.useCallback(() => {
        fetch(`/api/wakeup`).then(async (res) => {
            const data = await res.json();
            setIdleTime(data.idle_time ?? 0);
            setWakeStatus(data.wake_on ?? false);
        });
    }, []);

    React.useEffect(() => {
        let alive = true;

        const tick = async () => {
            if (!alive) return;

            await loadMore();

            if (alive) {
                setTimeout(tick, 500);
            }
        };

        tick();

        return () => {
            alive = false;
        };
    }, [loadMore]);

    return (
        <div style={{ padding: 16, fontFamily: "system-ui" }}>
            <h2>Image Gallery</h2>
            <div style={{ height: 12 }} />
            <text>Idle time: {idleTime.toFixed(2)} seconds</text>
            <div style={{ height: 12 }} />
            <text>Wake status: {wakeStatus ? "ON" : "OFF"}</text>
            <div style={{ height: 12 }} />
            <button onClick={sendWakeUpCall}>Send Wake Up Call</button>
        </div >
    );
}
