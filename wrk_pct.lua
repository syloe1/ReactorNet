-- wrk 百分位脚本：输出 P50/P90/P95/P99
-- wrk 的 latency 对象内部单位是微秒(µs)，除以 1000 转毫秒
local function report(summary, latency)
    io.write("----------------------------------------\n")
    io.write(string.format("  Requests/sec : %10.2f\n", summary.requests / summary.duration))
    io.write(string.format("  Avg latency  : %10.2f ms\n", latency.mean / 1000.0))
    io.write(string.format("  P50          : %10.2f ms\n", latency:percentile(50.0) / 1000.0))
    io.write(string.format("  P90          : %10.2f ms\n", latency:percentile(90.0) / 1000.0))
    io.write(string.format("  P95          : %10.2f ms\n", latency:percentile(95.0) / 1000.0))
    io.write(string.format("  P99          : %10.2f ms\n", latency:percentile(99.0) / 1000.0))
    io.write("----------------------------------------\n")
end

done = function(summary, latency, requests)
    report(summary, latency)
end
