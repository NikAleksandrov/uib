# Reference transport benchmark

This is one complete `make benchmark-report` run inside the virtme-ng guest.
It is a reproducible functional and comparative baseline, not a hardware
performance claim.

- Date: 2026-08-20
- Environment: virtme-ng guest, 2 virtual CPUs, 2 GiB RAM
- Host CPU: AMD Ryzen 9 3950X, x86-64
- Workload: 1,000 messages, 16,384 bytes per message, window 16
- MTUs: 1500, 4096, and 8192 bytes where the protocol permits them
- Affinity: unpinned and pinned to one allowed guest CPU
- Result: all 128 available UDP/TCP combinations passed payload verification
- Unavailable: AF_XDP had no configured interface; RoCEv2 8K is invalid

The generated tables below contain actual measurements for bandwidth, latency,
CPU time, CPU utilization, zero-copy requests, DMA-BUF, affinity, and MTU.
Percentage comparisons follow the complete value table.

# UIB benchmark matrix

| Protocol | Transport | Memory | MTU | Affinity | Send mode | Gbit/s | MiB/s | CPU s | CPU % | Average us | p50 us | p95 us | p99 us | Deferred/requests |
|---|---|---|---:|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| rocev2 | UDP | host | 1500 | unpinned | copy | 1.473 | 175.6 | 0.156 | 175.3 | 633.8 | 560.8 | 1293.0 | 1477.1 | 0/0 |
| rocev2 | UDP | host | 1500 | unpinned | `MSG_ZEROCOPY` | 0.223 | 26.6 | 0.202 | 34.3 | 5262.2 | 819.5 | 1341.0 | 247146.9 | 11182/11182 |
| rocev2 | TCP | host | 1500 | unpinned | copy | 1.435 | 171.0 | 0.163 | 178.7 | 746.7 | 708.3 | 1292.0 | 1569.3 | 0/0 |
| rocev2 | TCP | host | 1500 | unpinned | `MSG_ZEROCOPY` | 0.601 | 71.6 | 0.242 | 111.1 | 1873.6 | 1832.9 | 3404.5 | 3461.9 | 11000/11000 |
| rocev2 | UDP | dmabuf | 1500 | unpinned | copy | 1.489 | 177.5 | 0.155 | 176.3 | 624.4 | 554.1 | 1288.7 | 1365.1 | 0/0 |
| rocev2 | UDP | dmabuf | 1500 | unpinned | `MSG_ZEROCOPY` | 1.432 | 170.7 | 0.165 | 180.3 | 802.0 | 803.2 | 1275.5 | 1381.6 | 11000/11000 |
| rocev2 | TCP | dmabuf | 1500 | unpinned | copy | 1.451 | 173.0 | 0.164 | 181.0 | 794.9 | 770.2 | 1359.9 | 1524.4 | 0/0 |
| rocev2 | TCP | dmabuf | 1500 | unpinned | `MSG_ZEROCOPY` | 0.605 | 72.1 | 0.238 | 109.8 | 1856.5 | 1821.4 | 3365.2 | 3463.5 | 11000/11000 |
| uet | UDP | host | 1500 | unpinned | copy | 2.417 | 288.1 | 0.088 | 163.1 | 507.2 | 516.4 | 800.2 | 864.7 | 0/0 |
| uet | UDP | host | 1500 | unpinned | `MSG_ZEROCOPY` | 1.830 | 218.2 | 0.115 | 160.0 | 662.3 | 694.8 | 995.0 | 1135.8 | 11000/11000 |
| uet | TCP | host | 1500 | unpinned | copy | 1.617 | 192.8 | 0.109 | 134.3 | 696.4 | 692.4 | 1172.3 | 1262.7 | 0/0 |
| uet | TCP | host | 1500 | unpinned | `MSG_ZEROCOPY` | 0.742 | 88.5 | 0.205 | 116.2 | 1474.8 | 1456.4 | 2669.3 | 3572.4 | 11000/11000 |
| uet | UDP | dmabuf | 1500 | unpinned | copy | 2.209 | 263.3 | 0.092 | 155.5 | 661.2 | 744.1 | 886.0 | 933.6 | 0/0 |
| uet | UDP | dmabuf | 1500 | unpinned | `MSG_ZEROCOPY` | 1.834 | 218.6 | 0.115 | 160.6 | 642.8 | 663.0 | 981.0 | 1023.4 | 11000/11000 |
| uet | TCP | dmabuf | 1500 | unpinned | copy | 1.556 | 185.5 | 0.115 | 136.1 | 702.6 | 679.5 | 1148.5 | 1577.6 | 0/0 |
| uet | TCP | dmabuf | 1500 | unpinned | `MSG_ZEROCOPY` | 0.742 | 88.5 | 0.206 | 116.5 | 1439.6 | 1410.0 | 2654.1 | 3020.5 | 11000/11000 |
| dummy | UDP | host | 1500 | unpinned | copy | 16.067 | 1915.3 | 0.008 | 96.3 | 37.1 | 36.3 | 71.9 | 91.3 | 0/0 |
| dummy | UDP | host | 1500 | unpinned | `MSG_ZEROCOPY` | 11.915 | 1420.4 | 0.013 | 117.1 | 63.0 | 62.8 | 117.4 | 127.9 | 1000/1000 |
| dummy | TCP | host | 1500 | unpinned | copy | 14.532 | 1732.4 | 0.009 | 104.2 | 44.8 | 44.8 | 84.1 | 90.3 | 0/0 |
| dummy | TCP | host | 1500 | unpinned | `MSG_ZEROCOPY` | 9.085 | 1083.0 | 0.017 | 117.5 | 117.8 | 114.7 | 220.4 | 265.8 | 1000/1000 |
| dummy | UDP | dmabuf | 1500 | unpinned | copy | 11.568 | 1379.0 | 0.014 | 125.4 | 64.0 | 62.9 | 119.4 | 136.1 | 0/0 |
| dummy | UDP | dmabuf | 1500 | unpinned | `MSG_ZEROCOPY` | 8.545 | 1018.7 | 0.023 | 147.2 | 97.2 | 95.5 | 182.8 | 198.3 | 1000/1000 |
| dummy | TCP | dmabuf | 1500 | unpinned | copy | 9.695 | 1155.7 | 0.018 | 132.5 | 82.3 | 81.1 | 152.6 | 181.3 | 0/0 |
| dummy | TCP | dmabuf | 1500 | unpinned | `MSG_ZEROCOPY` | 9.317 | 1110.6 | 0.017 | 119.4 | 117.4 | 116.8 | 218.1 | 238.4 | 1000/1000 |
| rocev2 | UDP | host | 4096 | unpinned | copy | 2.192 | 261.3 | 0.108 | 180.5 | 496.8 | 485.8 | 846.7 | 885.9 | 0/0 |
| rocev2 | UDP | host | 4096 | unpinned | `MSG_ZEROCOPY` | 0.162 | 19.4 | 0.169 | 21.0 | 4022.0 | 558.6 | 973.9 | 247230.2 | 4047/4047 |
| rocev2 | TCP | host | 4096 | unpinned | copy | 2.122 | 253.0 | 0.114 | 184.2 | 519.1 | 509.1 | 900.8 | 945.4 | 0/0 |
| rocev2 | TCP | host | 4096 | unpinned | `MSG_ZEROCOPY` | 1.040 | 123.9 | 0.133 | 105.5 | 1094.6 | 1105.9 | 1967.1 | 2079.8 | 4000/4000 |
| rocev2 | UDP | dmabuf | 4096 | unpinned | copy | 2.184 | 260.4 | 0.108 | 180.0 | 500.9 | 482.5 | 847.9 | 994.8 | 0/0 |
| rocev2 | UDP | dmabuf | 4096 | unpinned | `MSG_ZEROCOPY` | 0.127 | 15.1 | 0.183 | 17.6 | 5405.9 | 536.6 | 1100.6 | 249359.6 | 4068/4068 |
| rocev2 | TCP | dmabuf | 4096 | unpinned | copy | 2.161 | 257.6 | 0.112 | 184.7 | 521.3 | 523.2 | 896.9 | 932.4 | 0/0 |
| rocev2 | TCP | dmabuf | 4096 | unpinned | `MSG_ZEROCOPY` | 1.004 | 119.7 | 0.138 | 105.5 | 1125.0 | 1122.6 | 2019.9 | 2485.4 | 4000/4000 |
| uet | UDP | host | 4096 | unpinned | copy | 3.596 | 428.7 | 0.058 | 159.2 | 359.9 | 385.6 | 517.6 | 593.4 | 0/0 |
| uet | UDP | host | 4096 | unpinned | `MSG_ZEROCOPY` | 3.462 | 412.6 | 0.060 | 157.3 | 329.3 | 323.9 | 456.1 | 523.7 | 4000/4000 |
| uet | TCP | host | 4096 | unpinned | copy | 4.333 | 516.5 | 0.048 | 157.3 | 297.9 | 287.4 | 479.9 | 548.9 | 0/0 |
| uet | TCP | host | 4096 | unpinned | `MSG_ZEROCOPY` | 1.793 | 213.7 | 0.087 | 118.8 | 599.4 | 584.8 | 1090.0 | 1156.1 | 4000/4000 |
| uet | UDP | dmabuf | 4096 | unpinned | copy | 5.251 | 625.9 | 0.038 | 150.3 | 226.8 | 233.8 | 316.1 | 350.5 | 0/0 |
| uet | UDP | dmabuf | 4096 | unpinned | `MSG_ZEROCOPY` | 4.648 | 554.1 | 0.043 | 151.2 | 249.9 | 250.3 | 332.6 | 380.1 | 4000/4000 |
| uet | TCP | dmabuf | 4096 | unpinned | copy | 4.853 | 578.6 | 0.042 | 157.1 | 254.3 | 263.3 | 362.4 | 434.7 | 0/0 |
| uet | TCP | dmabuf | 4096 | unpinned | `MSG_ZEROCOPY` | 1.860 | 221.7 | 0.084 | 119.0 | 582.4 | 575.7 | 1040.9 | 1104.8 | 4000/4000 |
| dummy | UDP | host | 4096 | unpinned | copy | 12.129 | 1445.9 | 0.013 | 121.8 | 59.7 | 59.5 | 112.3 | 120.7 | 0/0 |
| dummy | UDP | host | 4096 | unpinned | `MSG_ZEROCOPY` | 9.021 | 1075.4 | 0.021 | 146.7 | 91.4 | 89.8 | 171.7 | 197.5 | 1000/1000 |
| dummy | TCP | host | 4096 | unpinned | copy | 12.597 | 1501.6 | 0.012 | 114.5 | 56.4 | 50.9 | 135.9 | 179.3 | 0/0 |
| dummy | TCP | host | 4096 | unpinned | `MSG_ZEROCOPY` | 9.270 | 1105.1 | 0.017 | 116.9 | 117.3 | 114.9 | 219.6 | 236.8 | 1000/1000 |
| dummy | UDP | dmabuf | 4096 | unpinned | copy | 17.616 | 2100.0 | 0.007 | 98.8 | 36.7 | 35.2 | 67.1 | 92.8 | 0/0 |
| dummy | UDP | dmabuf | 4096 | unpinned | `MSG_ZEROCOPY` | 11.456 | 1365.6 | 0.013 | 111.5 | 64.7 | 63.0 | 118.9 | 170.5 | 1000/1000 |
| dummy | TCP | dmabuf | 4096 | unpinned | copy | 13.091 | 1560.5 | 0.011 | 108.7 | 51.8 | 48.2 | 101.3 | 191.7 | 0/0 |
| dummy | TCP | dmabuf | 4096 | unpinned | `MSG_ZEROCOPY` | 9.214 | 1098.4 | 0.017 | 118.8 | 117.1 | 117.9 | 218.2 | 256.1 | 1000/1000 |
| uet | UDP | host | 8192 | unpinned | copy | 6.517 | 776.9 | 0.029 | 143.9 | 171.4 | 162.5 | 280.6 | 354.2 | 0/0 |
| uet | UDP | host | 8192 | unpinned | `MSG_ZEROCOPY` | 5.738 | 684.0 | 0.031 | 137.6 | 188.6 | 185.6 | 287.9 | 339.1 | 2000/2000 |
| uet | TCP | host | 8192 | unpinned | copy | 4.310 | 513.8 | 0.047 | 155.7 | 243.1 | 234.7 | 398.7 | 458.5 | 0/0 |
| uet | TCP | host | 8192 | unpinned | `MSG_ZEROCOPY` | 1.928 | 229.9 | 0.083 | 122.5 | 568.1 | 553.2 | 1034.8 | 1159.2 | 2000/2000 |
| uet | UDP | dmabuf | 8192 | unpinned | copy | 6.717 | 800.7 | 0.028 | 141.2 | 158.2 | 154.3 | 241.7 | 300.7 | 0/0 |
| uet | UDP | dmabuf | 8192 | unpinned | `MSG_ZEROCOPY` | 6.051 | 721.3 | 0.032 | 146.2 | 171.4 | 168.7 | 265.6 | 308.8 | 2000/2000 |
| uet | TCP | dmabuf | 8192 | unpinned | copy | 6.460 | 770.1 | 0.031 | 155.1 | 167.2 | 166.4 | 255.7 | 306.1 | 0/0 |
| uet | TCP | dmabuf | 8192 | unpinned | `MSG_ZEROCOPY` | 2.714 | 323.5 | 0.059 | 123.0 | 406.4 | 383.7 | 849.3 | 1027.8 | 2000/2000 |
| dummy | UDP | host | 8192 | unpinned | copy | 16.410 | 1956.2 | 0.008 | 94.6 | 36.9 | 36.5 | 69.0 | 74.1 | 0/0 |
| dummy | UDP | host | 8192 | unpinned | `MSG_ZEROCOPY` | 11.966 | 1426.5 | 0.012 | 109.7 | 61.7 | 61.1 | 114.7 | 133.5 | 1000/1000 |
| dummy | TCP | host | 8192 | unpinned | copy | 14.078 | 1678.2 | 0.010 | 104.2 | 46.7 | 46.8 | 88.1 | 101.7 | 0/0 |
| dummy | TCP | host | 8192 | unpinned | `MSG_ZEROCOPY` | 9.011 | 1074.2 | 0.017 | 117.1 | 118.7 | 120.3 | 219.0 | 250.3 | 1000/1000 |
| dummy | UDP | dmabuf | 8192 | unpinned | copy | 15.949 | 1901.2 | 0.008 | 99.1 | 37.7 | 36.1 | 68.9 | 123.2 | 0/0 |
| dummy | UDP | dmabuf | 8192 | unpinned | `MSG_ZEROCOPY` | 11.453 | 1365.3 | 0.013 | 113.8 | 64.8 | 63.0 | 119.9 | 160.1 | 1000/1000 |
| dummy | TCP | dmabuf | 8192 | unpinned | copy | 13.994 | 1668.2 | 0.010 | 105.7 | 47.3 | 45.4 | 87.0 | 110.5 | 0/0 |
| dummy | TCP | dmabuf | 8192 | unpinned | `MSG_ZEROCOPY` | 9.089 | 1083.5 | 0.017 | 115.5 | 119.6 | 118.2 | 220.7 | 247.2 | 1000/1000 |
| rocev2 | UDP | host | 1500 | pinned | copy | 1.028 | 122.5 | 0.127 | 99.7 | 1502.5 | 1517.8 | 1890.9 | 2012.9 | 0/0 |
| rocev2 | UDP | host | 1500 | pinned | `MSG_ZEROCOPY` | 0.005 | 0.6 | 2.147 | 7.7 | 156597.9 | 249896.0 | 449795.2 | 449910.6 | 17425/17425 |
| rocev2 | TCP | host | 1500 | pinned | copy | 1.005 | 119.8 | 0.130 | 99.6 | 1530.7 | 1541.9 | 2152.2 | 2330.3 | 0/0 |
| rocev2 | TCP | host | 1500 | pinned | `MSG_ZEROCOPY` | 0.658 | 78.5 | 0.197 | 99.1 | 1667.0 | 1617.9 | 3101.1 | 3205.4 | 11000/11000 |
| rocev2 | UDP | dmabuf | 1500 | pinned | copy | 1.027 | 122.4 | 0.127 | 99.7 | 1500.2 | 1514.1 | 1869.7 | 1963.0 | 0/0 |
| rocev2 | UDP | dmabuf | 1500 | pinned | `MSG_ZEROCOPY` | 0.005 | 0.6 | 2.081 | 7.8 | 149453.6 | 248623.5 | 449852.7 | 449993.0 | 17103/17103 |
| rocev2 | TCP | dmabuf | 1500 | pinned | copy | 0.975 | 116.2 | 0.134 | 99.7 | 1593.1 | 1614.5 | 2223.8 | 2412.0 | 0/0 |
| rocev2 | TCP | dmabuf | 1500 | pinned | `MSG_ZEROCOPY` | 0.642 | 76.5 | 0.202 | 99.0 | 1705.9 | 1689.8 | 3173.4 | 3346.2 | 11000/11000 |
| uet | UDP | host | 1500 | pinned | copy | 1.171 | 139.6 | 0.110 | 98.6 | 1066.8 | 1095.6 | 1664.6 | 2248.7 | 0/0 |
| uet | UDP | host | 1500 | pinned | `MSG_ZEROCOPY` | 0.351 | 41.8 | 0.140 | 37.4 | 3886.3 | 1175.7 | 1853.7 | 250771.8 | 11055/11055 |
| uet | TCP | host | 1500 | pinned | copy | 1.182 | 141.0 | 0.109 | 98.5 | 1042.3 | 1074.5 | 1661.6 | 1761.8 | 0/0 |
| uet | TCP | host | 1500 | pinned | `MSG_ZEROCOPY` | 0.870 | 103.7 | 0.148 | 98.3 | 1251.3 | 1207.3 | 2330.7 | 2387.3 | 11000/11000 |
| uet | UDP | dmabuf | 1500 | pinned | copy | 1.258 | 150.0 | 0.103 | 98.5 | 988.6 | 1035.0 | 1547.2 | 1606.1 | 0/0 |
| uet | UDP | dmabuf | 1500 | pinned | `MSG_ZEROCOPY` | 0.346 | 41.2 | 0.145 | 38.2 | 3918.0 | 1198.5 | 1924.3 | 250776.8 | 11055/11055 |
| uet | TCP | dmabuf | 1500 | pinned | copy | 1.193 | 142.2 | 0.108 | 98.2 | 1034.2 | 1069.4 | 1649.3 | 1752.9 | 0/0 |
| uet | TCP | dmabuf | 1500 | pinned | `MSG_ZEROCOPY` | 0.868 | 103.4 | 0.148 | 98.3 | 1253.9 | 1255.0 | 2323.7 | 2424.8 | 11000/11000 |
| dummy | UDP | host | 1500 | pinned | copy | 19.402 | 2312.9 | 0.007 | 98.9 | 54.4 | 51.9 | 102.4 | 118.2 | 0/0 |
| dummy | UDP | host | 1500 | pinned | `MSG_ZEROCOPY` | 11.604 | 1383.3 | 0.010 | 88.7 | 70.5 | 64.6 | 152.3 | 168.3 | 1000/1000 |
| dummy | TCP | host | 1500 | pinned | copy | 13.145 | 1567.0 | 0.008 | 81.3 | 51.0 | 44.5 | 116.0 | 134.2 | 0/0 |
| dummy | TCP | host | 1500 | pinned | `MSG_ZEROCOPY` | 9.402 | 1120.8 | 0.013 | 93.5 | 107.7 | 107.3 | 200.7 | 225.4 | 1000/1000 |
| dummy | UDP | dmabuf | 1500 | pinned | copy | 16.467 | 1963.0 | 0.008 | 98.5 | 61.9 | 59.7 | 120.4 | 148.0 | 0/0 |
| dummy | UDP | dmabuf | 1500 | pinned | `MSG_ZEROCOPY` | 10.141 | 1209.0 | 0.012 | 91.5 | 85.1 | 78.0 | 185.0 | 233.4 | 1000/1000 |
| dummy | TCP | dmabuf | 1500 | pinned | copy | 13.218 | 1575.8 | 0.009 | 88.7 | 58.9 | 51.8 | 129.8 | 153.3 | 0/0 |
| dummy | TCP | dmabuf | 1500 | pinned | `MSG_ZEROCOPY` | 7.836 | 934.1 | 0.014 | 85.1 | 115.1 | 113.9 | 208.7 | 281.1 | 1000/1000 |
| rocev2 | UDP | host | 4096 | pinned | copy | 0.008 | 1.0 | 1.167 | 7.6 | 61461.1 | 1156.1 | 249442.8 | 249511.3 | 0/0 |
| rocev2 | UDP | host | 4096 | pinned | `MSG_ZEROCOPY` | 0.008 | 1.0 | 1.172 | 7.6 | 75379.6 | 1206.7 | 249633.8 | 249706.8 | 5032/5032 |
| rocev2 | TCP | host | 4096 | pinned | copy | 1.342 | 160.0 | 0.097 | 99.7 | 1185.3 | 1179.3 | 1577.9 | 1676.6 | 0/0 |
| rocev2 | TCP | host | 4096 | pinned | `MSG_ZEROCOPY` | 1.099 | 131.0 | 0.119 | 99.5 | 1002.5 | 1006.9 | 1862.4 | 1951.3 | 4000/4000 |
| rocev2 | UDP | dmabuf | 4096 | pinned | copy | 0.008 | 1.0 | 1.160 | 7.5 | 60459.2 | 1149.9 | 249414.3 | 249503.2 | 0/0 |
| rocev2 | UDP | dmabuf | 4096 | pinned | `MSG_ZEROCOPY` | 0.008 | 1.0 | 1.173 | 7.6 | 74645.5 | 1220.0 | 249651.1 | 249717.2 | 5031/5031 |
| rocev2 | TCP | dmabuf | 4096 | pinned | copy | 1.352 | 161.2 | 0.097 | 99.7 | 1166.2 | 1162.2 | 1564.8 | 1632.3 | 0/0 |
| rocev2 | TCP | dmabuf | 4096 | pinned | `MSG_ZEROCOPY` | 1.111 | 132.5 | 0.117 | 98.9 | 989.6 | 997.5 | 1838.4 | 1911.0 | 4000/4000 |
| uet | UDP | host | 4096 | pinned | copy | 0.452 | 53.9 | 0.055 | 19.1 | 1340.8 | 391.7 | 567.9 | 616.3 | 0/0 |
| uet | UDP | host | 4096 | pinned | `MSG_ZEROCOPY` | 0.446 | 53.2 | 0.060 | 20.3 | 1673.6 | 423.4 | 617.0 | 676.6 | 4018/4018 |
| uet | TCP | host | 4096 | pinned | copy | 3.248 | 387.2 | 0.039 | 95.6 | 395.9 | 396.5 | 577.2 | 643.7 | 0/0 |
| uet | TCP | host | 4096 | pinned | `MSG_ZEROCOPY` | 1.983 | 236.4 | 0.064 | 96.4 | 533.1 | 518.2 | 985.1 | 1023.1 | 4000/4000 |
| uet | UDP | dmabuf | 4096 | pinned | copy | 0.452 | 53.9 | 0.056 | 19.2 | 1339.5 | 393.8 | 552.3 | 635.2 | 0/0 |
| uet | UDP | dmabuf | 4096 | pinned | `MSG_ZEROCOPY` | 0.444 | 52.9 | 0.062 | 20.8 | 1678.5 | 433.4 | 643.6 | 735.5 | 4018/4018 |
| uet | TCP | dmabuf | 4096 | pinned | copy | 3.125 | 372.6 | 0.040 | 96.1 | 410.4 | 397.3 | 637.7 | 745.8 | 0/0 |
| uet | TCP | dmabuf | 4096 | pinned | `MSG_ZEROCOPY` | 1.975 | 235.5 | 0.064 | 96.7 | 539.5 | 534.9 | 996.9 | 1036.8 | 4000/4000 |
| dummy | UDP | host | 4096 | pinned | copy | 18.996 | 2264.5 | 0.007 | 99.3 | 56.2 | 53.3 | 104.5 | 127.6 | 0/0 |
| dummy | UDP | host | 4096 | pinned | `MSG_ZEROCOPY` | 11.915 | 1420.4 | 0.010 | 89.6 | 69.0 | 62.2 | 152.0 | 168.9 | 1000/1000 |
| dummy | TCP | host | 4096 | pinned | copy | 13.506 | 1610.1 | 0.008 | 84.3 | 52.5 | 45.9 | 117.8 | 136.4 | 0/0 |
| dummy | TCP | host | 4096 | pinned | `MSG_ZEROCOPY` | 9.326 | 1111.8 | 0.013 | 94.5 | 110.1 | 109.9 | 204.8 | 231.9 | 1000/1000 |
| dummy | UDP | dmabuf | 4096 | pinned | copy | 17.869 | 2130.2 | 0.007 | 97.0 | 56.9 | 55.5 | 105.8 | 142.8 | 0/0 |
| dummy | UDP | dmabuf | 4096 | pinned | `MSG_ZEROCOPY` | 11.569 | 1379.1 | 0.010 | 89.6 | 70.8 | 63.2 | 152.4 | 177.1 | 1000/1000 |
| dummy | TCP | dmabuf | 4096 | pinned | copy | 12.864 | 1533.5 | 0.008 | 81.6 | 51.3 | 44.6 | 115.1 | 134.2 | 0/0 |
| dummy | TCP | dmabuf | 4096 | pinned | `MSG_ZEROCOPY` | 8.993 | 1072.1 | 0.014 | 93.7 | 112.0 | 111.5 | 206.1 | 242.5 | 1000/1000 |
| uet | UDP | host | 8192 | pinned | copy | 0.476 | 56.8 | 0.041 | 15.1 | 1243.9 | 292.5 | 342.8 | 413.1 | 0/0 |
| uet | UDP | host | 8192 | pinned | `MSG_ZEROCOPY` | 4.387 | 522.9 | 0.029 | 97.1 | 311.7 | 307.4 | 407.1 | 483.0 | 2000/2000 |
| uet | TCP | host | 8192 | pinned | copy | 4.982 | 593.9 | 0.025 | 93.7 | 298.8 | 292.3 | 376.2 | 410.5 | 0/0 |
| uet | TCP | host | 8192 | pinned | `MSG_ZEROCOPY` | 3.158 | 376.4 | 0.039 | 93.9 | 326.2 | 323.8 | 596.2 | 632.6 | 2000/2000 |
| uet | UDP | dmabuf | 8192 | pinned | copy | 0.475 | 56.7 | 0.042 | 15.2 | 1249.3 | 296.3 | 350.2 | 503.8 | 0/0 |
| uet | UDP | dmabuf | 8192 | pinned | `MSG_ZEROCOPY` | 4.398 | 524.3 | 0.029 | 98.1 | 322.5 | 318.7 | 434.8 | 548.9 | 2000/2000 |
| uet | TCP | dmabuf | 8192 | pinned | copy | 4.836 | 576.6 | 0.026 | 94.9 | 307.0 | 305.2 | 387.6 | 426.9 | 0/0 |
| uet | TCP | dmabuf | 8192 | pinned | `MSG_ZEROCOPY` | 3.087 | 368.0 | 0.040 | 94.0 | 334.4 | 328.4 | 610.1 | 657.7 | 2000/2000 |
| dummy | UDP | host | 8192 | pinned | copy | 19.700 | 2348.4 | 0.007 | 98.9 | 54.0 | 52.3 | 101.0 | 108.1 | 0/0 |
| dummy | UDP | host | 8192 | pinned | `MSG_ZEROCOPY` | 11.772 | 1403.3 | 0.010 | 88.7 | 68.7 | 61.8 | 150.1 | 168.3 | 1000/1000 |
| dummy | TCP | host | 8192 | pinned | copy | 13.896 | 1656.5 | 0.008 | 84.0 | 50.4 | 43.7 | 115.3 | 133.1 | 0/0 |
| dummy | TCP | host | 8192 | pinned | `MSG_ZEROCOPY` | 8.953 | 1067.3 | 0.013 | 92.1 | 111.1 | 110.8 | 207.5 | 219.8 | 1000/1000 |
| dummy | UDP | dmabuf | 8192 | pinned | copy | 18.504 | 2205.9 | 0.007 | 97.6 | 55.9 | 55.4 | 103.8 | 126.1 | 0/0 |
| dummy | UDP | dmabuf | 8192 | pinned | `MSG_ZEROCOPY` | 11.726 | 1397.8 | 0.010 | 90.3 | 71.2 | 63.6 | 152.6 | 175.6 | 1000/1000 |
| dummy | TCP | dmabuf | 8192 | pinned | copy | 13.394 | 1596.7 | 0.008 | 86.5 | 56.6 | 49.9 | 125.8 | 140.7 | 0/0 |
| dummy | TCP | dmabuf | 8192 | pinned | `MSG_ZEROCOPY` | 9.142 | 1089.8 | 0.013 | 93.0 | 110.6 | 110.2 | 203.0 | 224.1 | 1000/1000 |

## Zero-copy compared with copy

Positive values mean improvement.

| Protocol | Memory | Transport | MTU | Affinity | Copy Gbit/s | Zero-copy Gbit/s | Change | Copy latency us | Zero-copy latency us | Reduction | Copy CPU s | Zero-copy CPU s | Reduction | Deferred |
|---|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| rocev2 | host | UDP | 1500 | unpinned | 1.473 | 0.223 | -84.9% | 633.8 | 5262.2 | -730.2% | 0.156 | 0.202 | -29.3% | 100.0% |
| rocev2 | host | TCP | 1500 | unpinned | 1.435 | 0.601 | -58.1% | 746.7 | 1873.6 | -150.9% | 0.163 | 0.242 | -48.5% | 100.0% |
| rocev2 | dmabuf | UDP | 1500 | unpinned | 1.489 | 1.432 | -3.8% | 624.4 | 802.0 | -28.4% | 0.155 | 0.165 | -6.3% | 100.0% |
| rocev2 | dmabuf | TCP | 1500 | unpinned | 1.451 | 0.605 | -58.3% | 794.9 | 1856.5 | -133.6% | 0.164 | 0.238 | -45.5% | 100.0% |
| uet | host | UDP | 1500 | unpinned | 2.417 | 1.830 | -24.3% | 507.2 | 662.3 | -30.6% | 0.088 | 0.115 | -29.5% | 100.0% |
| uet | host | TCP | 1500 | unpinned | 1.617 | 0.742 | -54.1% | 696.4 | 1474.8 | -111.8% | 0.109 | 0.205 | -88.6% | 100.0% |
| uet | dmabuf | UDP | 1500 | unpinned | 2.209 | 1.834 | -17.0% | 661.2 | 642.8 | +2.8% | 0.092 | 0.115 | -24.4% | 100.0% |
| uet | dmabuf | TCP | 1500 | unpinned | 1.556 | 0.742 | -52.3% | 702.6 | 1439.6 | -104.9% | 0.115 | 0.206 | -79.4% | 100.0% |
| dummy | host | UDP | 1500 | unpinned | 16.067 | 11.915 | -25.8% | 37.1 | 63.0 | -69.7% | 0.008 | 0.013 | -64.0% | 100.0% |
| dummy | host | TCP | 1500 | unpinned | 14.532 | 9.085 | -37.5% | 44.8 | 117.8 | -163.1% | 0.009 | 0.017 | -80.5% | 100.0% |
| dummy | dmabuf | UDP | 1500 | unpinned | 11.568 | 8.545 | -26.1% | 64.0 | 97.2 | -51.7% | 0.014 | 0.023 | -58.9% | 100.0% |
| dummy | dmabuf | TCP | 1500 | unpinned | 9.695 | 9.317 | -3.9% | 82.3 | 117.4 | -42.7% | 0.018 | 0.017 | +6.2% | 100.0% |
| rocev2 | host | UDP | 4096 | unpinned | 2.192 | 0.162 | -92.6% | 496.8 | 4022.0 | -709.6% | 0.108 | 0.169 | -57.1% | 100.0% |
| rocev2 | host | TCP | 4096 | unpinned | 2.122 | 1.040 | -51.0% | 519.1 | 1094.6 | -110.9% | 0.114 | 0.133 | -17.0% | 100.0% |
| rocev2 | dmabuf | UDP | 4096 | unpinned | 2.184 | 0.127 | -94.2% | 500.9 | 5405.9 | -979.3% | 0.108 | 0.183 | -69.0% | 100.0% |
| rocev2 | dmabuf | TCP | 4096 | unpinned | 2.161 | 1.004 | -53.5% | 521.3 | 1125.0 | -115.8% | 0.112 | 0.138 | -22.9% | 100.0% |
| uet | host | UDP | 4096 | unpinned | 3.596 | 3.462 | -3.7% | 359.9 | 329.3 | +8.5% | 0.058 | 0.060 | -2.6% | 100.0% |
| uet | host | TCP | 4096 | unpinned | 4.333 | 1.793 | -58.6% | 297.9 | 599.4 | -101.2% | 0.048 | 0.087 | -82.6% | 100.0% |
| uet | dmabuf | UDP | 4096 | unpinned | 5.251 | 4.648 | -11.5% | 226.8 | 249.9 | -10.2% | 0.038 | 0.043 | -13.7% | 100.0% |
| uet | dmabuf | TCP | 4096 | unpinned | 4.853 | 1.860 | -61.7% | 254.3 | 582.4 | -129.0% | 0.042 | 0.084 | -97.7% | 100.0% |
| dummy | host | UDP | 4096 | unpinned | 12.129 | 9.021 | -25.6% | 59.7 | 91.4 | -53.2% | 0.013 | 0.021 | -61.9% | 100.0% |
| dummy | host | TCP | 4096 | unpinned | 12.597 | 9.270 | -26.4% | 56.4 | 117.3 | -107.9% | 0.012 | 0.017 | -38.8% | 100.0% |
| dummy | dmabuf | UDP | 4096 | unpinned | 17.616 | 11.456 | -35.0% | 36.7 | 64.7 | -76.4% | 0.007 | 0.013 | -73.6% | 100.0% |
| dummy | dmabuf | TCP | 4096 | unpinned | 13.091 | 9.214 | -29.6% | 51.8 | 117.1 | -126.2% | 0.011 | 0.017 | -55.3% | 100.0% |
| uet | host | UDP | 8192 | unpinned | 6.517 | 5.738 | -12.0% | 171.4 | 188.6 | -10.1% | 0.029 | 0.031 | -8.6% | 100.0% |
| uet | host | TCP | 8192 | unpinned | 4.310 | 1.928 | -55.3% | 243.1 | 568.1 | -133.7% | 0.047 | 0.083 | -75.8% | 100.0% |
| uet | dmabuf | UDP | 8192 | unpinned | 6.717 | 6.051 | -9.9% | 158.2 | 171.4 | -8.3% | 0.028 | 0.032 | -14.9% | 100.0% |
| uet | dmabuf | TCP | 8192 | unpinned | 6.460 | 2.714 | -58.0% | 167.2 | 406.4 | -143.1% | 0.031 | 0.059 | -88.7% | 100.0% |
| dummy | host | UDP | 8192 | unpinned | 16.410 | 11.966 | -27.1% | 36.9 | 61.7 | -67.4% | 0.008 | 0.012 | -59.0% | 100.0% |
| dummy | host | TCP | 8192 | unpinned | 14.078 | 9.011 | -36.0% | 46.7 | 118.7 | -153.9% | 0.010 | 0.017 | -75.6% | 100.0% |
| dummy | dmabuf | UDP | 8192 | unpinned | 15.949 | 11.453 | -28.2% | 37.7 | 64.8 | -71.8% | 0.008 | 0.013 | -59.8% | 100.0% |
| dummy | dmabuf | TCP | 8192 | unpinned | 13.994 | 9.089 | -35.0% | 47.3 | 119.6 | -152.8% | 0.010 | 0.017 | -68.3% | 100.0% |
| rocev2 | host | UDP | 1500 | pinned | 1.028 | 0.005 | -99.5% | 1502.5 | 156597.9 | -10322.7% | 0.127 | 2.147 | -1589.3% | 100.0% |
| rocev2 | host | TCP | 1500 | pinned | 1.005 | 0.658 | -34.5% | 1530.7 | 1667.0 | -8.9% | 0.130 | 0.197 | -52.0% | 100.0% |
| rocev2 | dmabuf | UDP | 1500 | pinned | 1.027 | 0.005 | -99.5% | 1500.2 | 149453.6 | -9861.9% | 0.127 | 2.081 | -1535.5% | 100.0% |
| rocev2 | dmabuf | TCP | 1500 | pinned | 0.975 | 0.642 | -34.2% | 1593.1 | 1705.9 | -7.1% | 0.134 | 0.202 | -50.9% | 100.0% |
| uet | host | UDP | 1500 | pinned | 1.171 | 0.351 | -70.1% | 1066.8 | 3886.3 | -264.3% | 0.110 | 0.140 | -26.7% | 100.0% |
| uet | host | TCP | 1500 | pinned | 1.182 | 0.870 | -26.4% | 1042.3 | 1251.3 | -20.1% | 0.109 | 0.148 | -35.6% | 100.0% |
| uet | dmabuf | UDP | 1500 | pinned | 1.258 | 0.346 | -72.5% | 988.6 | 3918.0 | -296.3% | 0.103 | 0.145 | -41.2% | 100.0% |
| uet | dmabuf | TCP | 1500 | pinned | 1.193 | 0.868 | -27.3% | 1034.2 | 1253.9 | -21.2% | 0.108 | 0.148 | -37.6% | 100.0% |
| dummy | host | UDP | 1500 | pinned | 19.402 | 11.604 | -40.2% | 54.4 | 70.5 | -29.7% | 0.007 | 0.010 | -49.9% | 100.0% |
| dummy | host | TCP | 1500 | pinned | 13.145 | 9.402 | -28.5% | 51.0 | 107.7 | -111.4% | 0.008 | 0.013 | -60.7% | 100.0% |
| dummy | dmabuf | UDP | 1500 | pinned | 16.467 | 10.141 | -38.4% | 61.9 | 85.1 | -37.5% | 0.008 | 0.012 | -50.8% | 100.0% |
| dummy | dmabuf | TCP | 1500 | pinned | 13.218 | 7.836 | -40.7% | 58.9 | 115.1 | -95.3% | 0.009 | 0.014 | -61.7% | 100.0% |
| rocev2 | host | UDP | 4096 | pinned | 0.008 | 0.008 | -0.1% | 61461.1 | 75379.6 | -22.6% | 1.167 | 1.172 | -0.3% | 100.0% |
| rocev2 | host | TCP | 4096 | pinned | 1.342 | 1.099 | -18.1% | 1185.3 | 1002.5 | +15.4% | 0.097 | 0.119 | -21.9% | 100.0% |
| rocev2 | dmabuf | UDP | 4096 | pinned | 0.008 | 0.008 | -0.1% | 60459.2 | 74645.5 | -23.5% | 1.160 | 1.173 | -1.1% | 100.0% |
| rocev2 | dmabuf | TCP | 4096 | pinned | 1.352 | 1.111 | -17.8% | 1166.2 | 989.6 | +15.1% | 0.097 | 0.117 | -20.8% | 100.0% |
| uet | host | UDP | 4096 | pinned | 0.452 | 0.446 | -1.4% | 1340.8 | 1673.6 | -24.8% | 0.055 | 0.060 | -7.6% | 100.0% |
| uet | host | TCP | 4096 | pinned | 3.248 | 1.983 | -38.9% | 395.9 | 533.1 | -34.7% | 0.039 | 0.064 | -65.1% | 100.0% |
| uet | dmabuf | UDP | 4096 | pinned | 0.452 | 0.444 | -1.9% | 1339.5 | 1678.5 | -25.3% | 0.056 | 0.062 | -10.3% | 100.0% |
| uet | dmabuf | TCP | 4096 | pinned | 3.125 | 1.975 | -36.8% | 410.4 | 539.5 | -31.5% | 0.040 | 0.064 | -59.2% | 100.0% |
| dummy | host | UDP | 4096 | pinned | 18.996 | 11.915 | -37.3% | 56.2 | 69.0 | -22.7% | 0.007 | 0.010 | -43.8% | 100.0% |
| dummy | host | TCP | 4096 | pinned | 13.506 | 9.326 | -30.9% | 52.5 | 110.1 | -109.6% | 0.008 | 0.013 | -62.3% | 100.0% |
| dummy | dmabuf | UDP | 4096 | pinned | 17.869 | 11.569 | -35.3% | 56.9 | 70.8 | -24.5% | 0.007 | 0.010 | -42.7% | 100.0% |
| dummy | dmabuf | TCP | 4096 | pinned | 12.864 | 8.993 | -30.1% | 51.3 | 112.0 | -118.3% | 0.008 | 0.014 | -64.2% | 100.0% |
| uet | host | UDP | 8192 | pinned | 0.476 | 4.387 | +820.8% | 1243.9 | 311.7 | +74.9% | 0.041 | 0.029 | +30.0% | 100.0% |
| uet | host | TCP | 8192 | pinned | 4.982 | 3.158 | -36.6% | 298.8 | 326.2 | -9.2% | 0.025 | 0.039 | -58.1% | 100.0% |
| uet | dmabuf | UDP | 8192 | pinned | 0.475 | 4.398 | +825.2% | 1249.3 | 322.5 | +74.2% | 0.042 | 0.029 | +30.2% | 100.0% |
| uet | dmabuf | TCP | 8192 | pinned | 4.836 | 3.087 | -36.2% | 307.0 | 334.4 | -8.9% | 0.026 | 0.040 | -55.3% | 100.0% |
| dummy | host | UDP | 8192 | pinned | 19.700 | 11.772 | -40.2% | 54.0 | 68.7 | -27.3% | 0.007 | 0.010 | -50.0% | 100.0% |
| dummy | host | TCP | 8192 | pinned | 13.896 | 8.953 | -35.6% | 50.4 | 111.1 | -120.6% | 0.008 | 0.013 | -70.1% | 100.0% |
| dummy | dmabuf | UDP | 8192 | pinned | 18.504 | 11.726 | -36.6% | 55.9 | 71.2 | -27.4% | 0.007 | 0.010 | -45.9% | 100.0% |
| dummy | dmabuf | TCP | 8192 | pinned | 13.394 | 9.142 | -31.7% | 56.6 | 110.6 | -95.5% | 0.008 | 0.013 | -57.6% | 100.0% |

## DMA-BUF compared with host memory

| Protocol | Mode | MTU | Affinity | Host Gbit/s | DMA-BUF Gbit/s | Change | Host latency us | DMA-BUF latency us | Change | Host CPU s | DMA-BUF CPU s | Change |
|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| rocev2 | udp-copy | 1500 | unpinned | 1.473 | 1.489 | +1.1% | 633.8 | 624.4 | -1.5% | 0.156 | 0.155 | -0.5% |
| rocev2 | udp-zerocopy | 1500 | unpinned | 0.223 | 1.432 | +541.9% | 5262.2 | 802.0 | -84.8% | 0.202 | 0.165 | -18.2% |
| rocev2 | tcp-copy | 1500 | unpinned | 1.435 | 1.451 | +1.2% | 746.7 | 794.9 | +6.5% | 0.163 | 0.164 | +0.2% |
| rocev2 | tcp-zerocopy | 1500 | unpinned | 0.601 | 0.605 | +0.7% | 1873.6 | 1856.5 | -0.9% | 0.242 | 0.238 | -1.9% |
| uet | udp-copy | 1500 | unpinned | 2.417 | 2.209 | -8.6% | 507.2 | 661.2 | +30.4% | 0.088 | 0.092 | +4.3% |
| uet | udp-zerocopy | 1500 | unpinned | 1.830 | 1.834 | +0.2% | 662.3 | 642.8 | -2.9% | 0.115 | 0.115 | +0.1% |
| uet | tcp-copy | 1500 | unpinned | 1.617 | 1.556 | -3.8% | 696.4 | 702.6 | +0.9% | 0.109 | 0.115 | +5.3% |
| uet | tcp-zerocopy | 1500 | unpinned | 0.742 | 0.742 | +0.0% | 1474.8 | 1439.6 | -2.4% | 0.205 | 0.206 | +0.2% |
| dummy | udp-copy | 1500 | unpinned | 16.067 | 11.568 | -28.0% | 37.1 | 64.0 | +72.5% | 0.008 | 0.014 | +80.8% |
| dummy | udp-zerocopy | 1500 | unpinned | 11.915 | 8.545 | -28.3% | 63.0 | 97.2 | +54.2% | 0.013 | 0.023 | +75.3% |
| dummy | tcp-copy | 1500 | unpinned | 14.532 | 9.695 | -33.3% | 44.8 | 82.3 | +83.7% | 0.009 | 0.018 | +90.7% |
| dummy | tcp-zerocopy | 1500 | unpinned | 9.085 | 9.317 | +2.5% | 117.8 | 117.4 | -0.4% | 0.017 | 0.017 | -0.9% |
| rocev2 | udp-copy | 4096 | unpinned | 2.192 | 2.184 | -0.4% | 496.8 | 500.9 | +0.8% | 0.108 | 0.108 | +0.1% |
| rocev2 | udp-zerocopy | 4096 | unpinned | 0.162 | 0.127 | -22.0% | 4022.0 | 5405.9 | +34.4% | 0.169 | 0.183 | +7.7% |
| rocev2 | tcp-copy | 4096 | unpinned | 2.122 | 2.161 | +1.8% | 519.1 | 521.3 | +0.4% | 0.114 | 0.112 | -1.5% |
| rocev2 | tcp-zerocopy | 4096 | unpinned | 1.040 | 1.004 | -3.4% | 1094.6 | 1125.0 | +2.8% | 0.133 | 0.138 | +3.5% |
| uet | udp-copy | 4096 | unpinned | 3.596 | 5.251 | +46.0% | 359.9 | 226.8 | -37.0% | 0.058 | 0.038 | -35.4% |
| uet | udp-zerocopy | 4096 | unpinned | 3.462 | 4.648 | +34.3% | 329.3 | 249.9 | -24.1% | 0.060 | 0.043 | -28.4% |
| uet | tcp-copy | 4096 | unpinned | 4.333 | 4.853 | +12.0% | 297.9 | 254.3 | -14.6% | 0.048 | 0.042 | -10.9% |
| uet | tcp-zerocopy | 4096 | unpinned | 1.793 | 1.860 | +3.8% | 599.4 | 582.4 | -2.8% | 0.087 | 0.084 | -3.5% |
| dummy | udp-copy | 4096 | unpinned | 12.129 | 17.616 | +45.2% | 59.7 | 36.7 | -38.6% | 0.013 | 0.007 | -44.2% |
| dummy | udp-zerocopy | 4096 | unpinned | 9.021 | 11.456 | +27.0% | 91.4 | 64.7 | -29.3% | 0.021 | 0.013 | -40.2% |
| dummy | tcp-copy | 4096 | unpinned | 12.597 | 13.091 | +3.9% | 56.4 | 51.8 | -8.2% | 0.012 | 0.011 | -8.6% |
| dummy | tcp-zerocopy | 4096 | unpinned | 9.270 | 9.214 | -0.6% | 117.3 | 117.1 | -0.1% | 0.017 | 0.017 | +2.2% |
| uet | udp-copy | 8192 | unpinned | 6.517 | 6.717 | +3.1% | 171.4 | 158.2 | -7.7% | 0.029 | 0.028 | -4.8% |
| uet | udp-zerocopy | 8192 | unpinned | 5.738 | 6.051 | +5.4% | 188.6 | 171.4 | -9.1% | 0.031 | 0.032 | +0.8% |
| uet | tcp-copy | 8192 | unpinned | 4.310 | 6.460 | +49.9% | 243.1 | 167.2 | -31.2% | 0.047 | 0.031 | -33.5% |
| uet | tcp-zerocopy | 8192 | unpinned | 1.928 | 2.714 | +40.8% | 568.1 | 406.4 | -28.5% | 0.083 | 0.059 | -28.7% |
| dummy | udp-copy | 8192 | unpinned | 16.410 | 15.949 | -2.8% | 36.9 | 37.7 | +2.4% | 0.008 | 0.008 | +7.8% |
| dummy | udp-zerocopy | 8192 | unpinned | 11.966 | 11.453 | -4.3% | 61.7 | 64.8 | +5.0% | 0.012 | 0.013 | +8.4% |
| dummy | tcp-copy | 8192 | unpinned | 14.078 | 13.994 | -0.6% | 46.7 | 47.3 | +1.2% | 0.010 | 0.010 | +2.1% |
| dummy | tcp-zerocopy | 8192 | unpinned | 9.011 | 9.089 | +0.9% | 118.7 | 119.6 | +0.8% | 0.017 | 0.017 | -2.2% |
| rocev2 | udp-copy | 1500 | pinned | 1.028 | 1.027 | -0.1% | 1502.5 | 1500.2 | -0.1% | 0.127 | 0.127 | +0.1% |
| rocev2 | udp-zerocopy | 1500 | pinned | 0.005 | 0.005 | +4.7% | 156597.9 | 149453.6 | -4.6% | 2.147 | 2.081 | -3.1% |
| rocev2 | tcp-copy | 1500 | pinned | 1.005 | 0.975 | -3.0% | 1530.7 | 1593.1 | +4.1% | 0.130 | 0.134 | +3.1% |
| rocev2 | tcp-zerocopy | 1500 | pinned | 0.658 | 0.642 | -2.5% | 1667.0 | 1705.9 | +2.3% | 0.197 | 0.202 | +2.4% |
| uet | udp-copy | 1500 | pinned | 1.171 | 1.258 | +7.4% | 1066.8 | 988.6 | -7.3% | 0.110 | 0.103 | -7.0% |
| uet | udp-zerocopy | 1500 | pinned | 0.351 | 0.346 | -1.3% | 3886.3 | 3918.0 | +0.8% | 0.140 | 0.145 | +3.6% |
| uet | tcp-copy | 1500 | pinned | 1.182 | 1.193 | +0.9% | 1042.3 | 1034.2 | -0.8% | 0.109 | 0.108 | -1.2% |
| uet | tcp-zerocopy | 1500 | pinned | 0.870 | 0.868 | -0.2% | 1251.3 | 1253.9 | +0.2% | 0.148 | 0.148 | +0.2% |
| dummy | udp-copy | 1500 | pinned | 19.402 | 16.467 | -15.1% | 54.4 | 61.9 | +13.9% | 0.007 | 0.008 | +17.3% |
| dummy | udp-zerocopy | 1500 | pinned | 11.604 | 10.141 | -12.6% | 70.5 | 85.1 | +20.8% | 0.010 | 0.012 | +18.0% |
| dummy | tcp-copy | 1500 | pinned | 13.145 | 13.218 | +0.6% | 51.0 | 58.9 | +15.7% | 0.008 | 0.009 | +8.5% |
| dummy | tcp-zerocopy | 1500 | pinned | 9.402 | 7.836 | -16.7% | 107.7 | 115.1 | +6.8% | 0.013 | 0.014 | +9.2% |
| rocev2 | udp-copy | 4096 | pinned | 0.008 | 0.008 | +0.0% | 61461.1 | 60459.2 | -1.6% | 1.167 | 1.160 | -0.6% |
| rocev2 | udp-zerocopy | 4096 | pinned | 0.008 | 0.008 | -0.0% | 75379.6 | 74645.5 | -1.0% | 1.172 | 1.173 | +0.1% |
| rocev2 | tcp-copy | 4096 | pinned | 1.342 | 1.352 | +0.8% | 1185.3 | 1166.2 | -1.6% | 0.097 | 0.097 | -0.7% |
| rocev2 | tcp-zerocopy | 4096 | pinned | 1.099 | 1.111 | +1.1% | 1002.5 | 989.6 | -1.3% | 0.119 | 0.117 | -1.6% |
| uet | udp-copy | 4096 | pinned | 0.452 | 0.452 | +0.0% | 1340.8 | 1339.5 | -0.1% | 0.055 | 0.056 | +0.6% |
| uet | udp-zerocopy | 4096 | pinned | 0.446 | 0.444 | -0.5% | 1673.6 | 1678.5 | +0.3% | 0.060 | 0.062 | +3.1% |
| uet | tcp-copy | 4096 | pinned | 3.248 | 3.125 | -3.8% | 395.9 | 410.4 | +3.7% | 0.039 | 0.040 | +4.4% |
| uet | tcp-zerocopy | 4096 | pinned | 1.983 | 1.975 | -0.4% | 533.1 | 539.5 | +1.2% | 0.064 | 0.064 | +0.7% |
| dummy | udp-copy | 4096 | pinned | 18.996 | 17.869 | -5.9% | 56.2 | 56.9 | +1.2% | 0.007 | 0.007 | +3.8% |
| dummy | udp-zerocopy | 4096 | pinned | 11.915 | 11.569 | -2.9% | 69.0 | 70.8 | +2.7% | 0.010 | 0.010 | +3.0% |
| dummy | tcp-copy | 4096 | pinned | 13.506 | 12.864 | -4.8% | 52.5 | 51.3 | -2.3% | 0.008 | 0.008 | +1.6% |
| dummy | tcp-zerocopy | 4096 | pinned | 9.326 | 8.993 | -3.6% | 110.1 | 112.0 | +1.7% | 0.013 | 0.014 | +2.8% |
| uet | udp-copy | 8192 | pinned | 0.476 | 0.475 | -0.2% | 1243.9 | 1249.3 | +0.4% | 0.041 | 0.042 | +1.1% |
| uet | udp-zerocopy | 8192 | pinned | 4.387 | 4.398 | +0.3% | 311.7 | 322.5 | +3.4% | 0.029 | 0.029 | +0.8% |
| uet | tcp-copy | 8192 | pinned | 4.982 | 4.836 | -2.9% | 298.8 | 307.0 | +2.7% | 0.025 | 0.026 | +4.3% |
| uet | tcp-zerocopy | 8192 | pinned | 3.158 | 3.087 | -2.2% | 326.2 | 334.4 | +2.5% | 0.039 | 0.040 | +2.5% |
| dummy | udp-copy | 8192 | pinned | 19.700 | 18.504 | -6.1% | 54.0 | 55.9 | +3.5% | 0.007 | 0.007 | +5.1% |
| dummy | udp-zerocopy | 8192 | pinned | 11.772 | 11.726 | -0.4% | 68.7 | 71.2 | +3.6% | 0.010 | 0.010 | +2.2% |
| dummy | tcp-copy | 8192 | pinned | 13.896 | 13.394 | -3.6% | 50.4 | 56.6 | +12.3% | 0.008 | 0.008 | +6.8% |
| dummy | tcp-zerocopy | 8192 | pinned | 8.953 | 9.142 | +2.1% | 111.1 | 110.6 | -0.5% | 0.013 | 0.013 | -1.1% |

## Skipped combinations

- afxdp-copy,rocev2,host,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,rocev2,host,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,rocev2,dmabuf,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,rocev2,dmabuf,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,uet,host,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,uet,host,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,uet,dmabuf,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,uet,dmabuf,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,dummy,host,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,dummy,host,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,dummy,dmabuf,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,dummy,dmabuf,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,rocev2,host,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,rocev2,host,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,rocev2,dmabuf,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,rocev2,dmabuf,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,uet,host,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,uet,host,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,uet,dmabuf,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,uet,dmabuf,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,dummy,host,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,dummy,host,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,dummy,dmabuf,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,dummy,dmabuf,unpinned: UIB_AFXDP_IFACE is not set
- udp-copy,rocev2,host,unpinned,mtu-8192: RoCEv2 path MTU maximum is 4096
- udp-zerocopy,rocev2,host,unpinned,mtu-8192: RoCEv2 path MTU maximum is 4096
- tcp-copy,rocev2,host,unpinned,mtu-8192: RoCEv2 path MTU maximum is 4096
- tcp-zerocopy,rocev2,host,unpinned,mtu-8192: RoCEv2 path MTU maximum is 4096
- afxdp-copy,rocev2,host,unpinned,mtu-8192: RoCEv2 path MTU maximum is 4096
- afxdp-zerocopy,rocev2,host,unpinned,mtu-8192: RoCEv2 path MTU maximum is 4096
- udp-copy,rocev2,dmabuf,unpinned,mtu-8192: RoCEv2 path MTU maximum is 4096
- udp-zerocopy,rocev2,dmabuf,unpinned,mtu-8192: RoCEv2 path MTU maximum is 4096
- tcp-copy,rocev2,dmabuf,unpinned,mtu-8192: RoCEv2 path MTU maximum is 4096
- tcp-zerocopy,rocev2,dmabuf,unpinned,mtu-8192: RoCEv2 path MTU maximum is 4096
- afxdp-copy,rocev2,dmabuf,unpinned,mtu-8192: RoCEv2 path MTU maximum is 4096
- afxdp-zerocopy,rocev2,dmabuf,unpinned,mtu-8192: RoCEv2 path MTU maximum is 4096
- afxdp-copy,uet,host,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,uet,host,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,uet,dmabuf,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,uet,dmabuf,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,dummy,host,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,dummy,host,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,dummy,dmabuf,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,dummy,dmabuf,unpinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,rocev2,host,pinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,rocev2,host,pinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,rocev2,dmabuf,pinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,rocev2,dmabuf,pinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,uet,host,pinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,uet,host,pinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,uet,dmabuf,pinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,uet,dmabuf,pinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,dummy,host,pinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,dummy,host,pinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,dummy,dmabuf,pinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,dummy,dmabuf,pinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,rocev2,host,pinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,rocev2,host,pinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,rocev2,dmabuf,pinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,rocev2,dmabuf,pinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,uet,host,pinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,uet,host,pinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,uet,dmabuf,pinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,uet,dmabuf,pinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,dummy,host,pinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,dummy,host,pinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,dummy,dmabuf,pinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,dummy,dmabuf,pinned: UIB_AFXDP_IFACE is not set
- udp-copy,rocev2,host,pinned,mtu-8192: RoCEv2 path MTU maximum is 4096
- udp-zerocopy,rocev2,host,pinned,mtu-8192: RoCEv2 path MTU maximum is 4096
- tcp-copy,rocev2,host,pinned,mtu-8192: RoCEv2 path MTU maximum is 4096
- tcp-zerocopy,rocev2,host,pinned,mtu-8192: RoCEv2 path MTU maximum is 4096
- afxdp-copy,rocev2,host,pinned,mtu-8192: RoCEv2 path MTU maximum is 4096
- afxdp-zerocopy,rocev2,host,pinned,mtu-8192: RoCEv2 path MTU maximum is 4096
- udp-copy,rocev2,dmabuf,pinned,mtu-8192: RoCEv2 path MTU maximum is 4096
- udp-zerocopy,rocev2,dmabuf,pinned,mtu-8192: RoCEv2 path MTU maximum is 4096
- tcp-copy,rocev2,dmabuf,pinned,mtu-8192: RoCEv2 path MTU maximum is 4096
- tcp-zerocopy,rocev2,dmabuf,pinned,mtu-8192: RoCEv2 path MTU maximum is 4096
- afxdp-copy,rocev2,dmabuf,pinned,mtu-8192: RoCEv2 path MTU maximum is 4096
- afxdp-zerocopy,rocev2,dmabuf,pinned,mtu-8192: RoCEv2 path MTU maximum is 4096
- afxdp-copy,uet,host,pinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,uet,host,pinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,uet,dmabuf,pinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,uet,dmabuf,pinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,dummy,host,pinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,dummy,host,pinned: UIB_AFXDP_IFACE is not set
- afxdp-copy,dummy,dmabuf,pinned: UIB_AFXDP_IFACE is not set
- afxdp-zerocopy,dummy,dmabuf,pinned: UIB_AFXDP_IFACE is not set
