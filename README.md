
# 滑动窗口协议

## Go-Back-N 协议

### 软件设计

Go-Back-N 协议由 `gobackn.c` 和 `common.h` 组成. 其中数据结构以及变量作用均有详细的注释. 例如帧的结构:

```text
data frame
+---------+--------+--------+-----------+----------+
| kind(1) | ack(1) | seq(1) | data(256) | crc32(4) |
+---------+--------+--------+-----------+----------+

ack/nak frame
+---------+--------+----------+
| kind(1) | ack(1) | crc32(4) |
+---------+--------+----------+
```

以及核心的协议参数:

```c
// 最大 seq 值, seq = 0, 1, ..., MAX_SEQ
#define MAX_SEQ 7

// 数据帧超时时间
#define DATA_TIMEOUT_MS 2000

// piggyback ack 延迟超时时间
#define ACK_TIMEOUT_MS 500
```

其他的数据结构和函数信息可以参考源码.

主程序在接收到事件之后, 主要流程如下:

<img src="images/main-flow.drawio.svg" alt="主要流程"/>

以及计时器超时后的处理:

```c
case DATA_TIMEOUT:
    dbg_event("DATA frame <seq=%d> timeout\n", arg);

    next_frame_to_send = ack_expected;
    for (seq_t i = 0; i < buffer_len; i++) {
        send_data_frame(next_frame_to_send, frame_expected, buffer);
        inc(next_frame_to_send);
    }
    break;

case ACK_TIMEOUT:
    send_ack_frame(frame_expected);
    stop_ack_timer();
    break;
```

除了收发帧, 该协议还考虑到了层间的流量控制:

1. 与物理层之间的流量控制
    - 每次调用 `send_frame()` 后将全局变量 `phl_ready` 置 `false`
    - 物理层发送队列低于 50 字节水平时，会产生 `PHYSICAL_LAYER_READY` 事件, 此时将 `phl_ready` 置 `true`
    - 若 `phl_ready` 为 `false`, 禁用网络层, 不再接收 packet

2. 与网络层之间的流量控制
   - 只要 `phl_ready` 为 `true` 且 packet buffer 未满, 则允许网络层发送 packet
   - 网络层若有 packet 需要发送, 则会产生 `NETWORK_LAYER_READY` 事件, 此时将 packet 存入 buffer 并发送 (详细过程见「主要流程」)

### 结果分析

经过测试, 本协议实现了有误码信道环境中无差错传输功能, 可以可靠地长时间运行.

#### 理论值分析

无差错信道上最大信道利用率为 `256 / (3 + 256 + 4) = 97.3%`. 在误码率为 1e-5 的信道上, 传输 `100000 / ((3 + 256 + 4) * 8) = 47` 个帧会有一个错误, 此时需要重传 `n (1 <= n <= MAX_SEQ + 1)` 次 (假设重传的帧不会出错). 在本协议中, `MAX_SEQ = 7`, 平均重传次数为 4.5 次, 故信道利用率为 `(256 * 47) / ((3 + 256 + 4) * (47 + 4.5)) = 88.8%`.

#### 参数选择

基于实验模拟的环境 (8000bps 全双工, 延迟 270ms, 误码率 1e-5), 对于 `MAX_SEQ`, `DATA_TIMEOUT_MS` 和 `ACK_TIMEOUT_MS` 这三个参数的选择, 分析如下:

1. `MAX_SEQ`: 滑动窗口的大小涉及到信道利用率和流量控制问题. 如果窗口太小, 流水线无法填满, 导致信道利用率低; 如果窗口太大, 会导致接收方处理不过来, 导致数据丢失. 窗口大小的一个下界是 `(2 * (帧传输时间 + 线路延迟) + t) / 帧传输时间`, 其中 t 是 `ACK_TIMEOUT_MS`.

2. `DATA_TIMEOUT_MS`: 重传定时器的设置要考虑到必要的传输时间和处理时间. 另外由于本协议使用了 NAK, 这个时限可以适当放宽. 这个时限的下界至少是 `2 * (帧传输时间 + 线路延迟) + t`.

3. `ACK_TIMEOUT_MS`: ACK 定时器的下界应该为网络层产生一个 packet 的时间, 由于这个时间无法准确得知, 所以只能通过实验获取一个经验值, 约为 500 ms.

结合理论分析和实践观察, 确定了最优值为 `MAX_SEQ = 7`, `DATA_TIMEOUT_MS = 2000`, `ACK_TIMEOUT_MS = 500`.

#### 测试结果

下表为测试 15 分钟得到的利用率, 其中 `命令` 列省略了站点 B 的命令 (和站点 A 参数一致).

| 命令                     | 说明                                                 | 站点 A 利用率 | 站点 B 利用率 | 理论值   |
|------------------------|----------------------------------------------------|----------|----------|-------|
| `gobackn a -u`         | 无误码信道, 站点 A 平缓发出数据, 站点 B 以「发送 100 秒，慢发 100 秒」周期性交替 | 51.59%   | 96.97%   | NA    |
| `gobackn a -fu`        | 无误码信道, 洪水式产生 packet                                | 96.97%   | 96.97%   | 97.3% |
| `gobackn a -f`         | 洪水式产生 packet                                       | 87.06%   | 87.01%   | 88.8% |
| `gobackn a`            | 默认, 站点 A 平缓发出数据, 站点 B 以「发送 100 秒，慢发 100 秒」周期性交替    | 47.14%   | 88.22%   | NA    |
| `gobackn a -f -b 1e-4` | 洪水式产生 packet, 线路误码率设为 1e-4                         | 37.41%   | 35.14%   | 49.7% |

可以看出, 协议在多个场景下均与理论值相差不大. 其中误码率设为 1e-4 时效率相差较大, 主要原因是假设了重传的帧不会出错, 以及忽略了层间传输和缓冲区的延迟. 进一步的改进可以通过 `phl_sq_len()` 监测物理层的缓冲区长度, 纳入延迟考量, 为每个帧设定恰当的超时时间. 或者改进帧的结构, 让其携带自身站点的拥堵信息, 供发送方控制流量.

## Selective Repeat 协议

### 软件设计

### 结果分析

## 研究性讨论

### 10.1 CRC 校验能力

- 假设本次实验中所设计的协议用于建设一个通信系统。这种“在有误码的信道上实现无差错传输”的功能听起来很不错, 但是后来该用户听说 CRC 校验理论上不可能 100%检出所有错误。这的确是事实, 你怎样说服他相信你的系统能够实现无差错传输？

  - 长 L bit 的校验和可以取 $2^L$ 个值, 含有它的信息包含没有被检出错误的概率为 $\frac{1}{2^L}$, 即任何随机差错被检出的概率都是 $1-\frac{1}{2^L}$。对 CRC-32 来说, 由于校验和长度为 32bit, 对全部类型的差错可以达到 99.9999%的检测率; 此外, 在一些特定情形下, CRC 可以达到 100%的检出率。对 CRC-32 来说, 它可以百分百检出以下差错类型：

    - 有一位错的信息

    - 有两位差错的信息

    - 有奇数个位差错的信息

    - 有和校验和一样长的突发差错的信息

  - 证明：

    对多项式 $s(D)$ 使用阶为 $L$ 的生成多项式 $g(D)$ 做带余除法, 记商式为 $z(D)$, 余式 $c(D)$ 可以被表示为

    $$
    \tag{10.1.1}
    s(D)D^L = g(D)z(D) + c(D)
    $$

    两边同时减去 $c(D)$ (模 2) 且注意到模 2 减法和模 2 加法是等价的, 我们有

    $$
    \tag{10.1.2}
    x(D) = s(D)D^L + C(D) = g(D)z(D)
    $$

    可以看到所有的合法码字都可以被 $g(D)$ 整除, 且所有可以被 $g(D)$ 整除的多项式都是合法码字。

    现在假设 $x(D)$ 被传输, 接收方收到的序列被表示为多项式 $y(D)$, 因为传输的时候出现了差错, $x(D)$ 和 $y(D)$ 不完全一致。如果将差错序列表示为多项式 $e(D)$, 则有 $y(D) = x(D) + e(D)$, $+$ 指模 2 加法。帧中的每个差错都可以表示为 $e(D)$ 中对应位置的系数。在接收方, 余式为 $\left[\frac{y(D)}{g(D)}\right]$。由于 $x(D)$ 可以被 $g(D)$ 整除,所以有

    $$
    \tag{10.1.3}
    Remainder \left[\frac{y(D)}{g(D)}\right] = Remainder \left[\frac{e(D)}{g(D)}\right]
    $$

    如果没有差错产生,则 $e(D) = 0$, 上述余式为 0。接收方遵循着这样的规则：当差错检测得到的余式为 0 的时候,认为收到的信息是无差错的;反之则认为是有差错的。当差错发生时 $[i.e., e(D) \neq 0]$, 仅当这个余式为 0 的时候接收方才会无法检测出差错。这种情况仅当 $e(D)$ 本身也是某个合法码字的时候才会发生。也就是说, $e(D) \neq 0$ 当且仅当存在非零多项式 $z(D)$, 使得

    $$
    \tag{10.1.4}
    e(D) = g(D)z(D)
    $$

    的情况下才是无法检测的。接下来进一步研究在什么情况下,无法检测的差错会出现。

    首先, 假设出现在一位差错 $e_i = 1$, 则有 $e(D) = D^i$。由于 $g(D)$ 有至少两个非零项 $(i.e., D^L 和 1)$, 对任意非零的 $z(D)$, $g(D)z(D)$ 都必有至少两个非零项。因此 $g(D)z(D)$ 不可能等于 $D^i$。由于对任意 i 都成立, 故所有的一位错误都可以被检测到, 即所有**有一位差错的信息都可以被检出**。类似的, 由于 $g(D)$ 中最高阶和最低阶相差 $L$ $(i.e., D^L 和 1, 相对的)$, 对所有非零的 $z(D)$, $g(D)z(D)$ 的最高阶项和最低阶项相差至少 $L$。因此, **如果 $e(D)$ 是合法码字, 那么突发错误的长度必须为至少 $L+1$** (突发错误长度为从第一个错误到最后一个错误(包括自身在内)的长度)。

    其次, 假设出现了两个差错, 分别在位置 $i$ 和 $j$, 则有

    $$
    \tag{10.1.5}
    e(D) = D^i + D^j = D^j(D^{i-j}+1),i>j
    $$

    根据前面的讨论, $D^j$ 不能被 $g(D)$ 或 $g(D)$ 的任意因式整除; 因此, 仅当 $D^{i-j}+1$ 可以被 $g(D)$ 整除时 $e(D)$ 才不会被检测到。对任意的度为 $L$ 的二进制多项式 $g(D)$, 存在最小的 $n$ 使得 $D^n+1$ 可以被 $g(D)$ 整除。根据有限域理论, 这个最小的 $n$ 不能大于 $2^L-1$; 此外, 对所有的 $L>0$, 存在特殊的阶为 $L$ 的多项式 _原始多项式( $primitive$ $polynomial$ )_, 使得这个最小的 $n$ 等于 $2^L-1$。因此, 如果所选的 $g(D)$ 是一个阶为 $L$ 的这样的多项式, 且帧长度被限制到最长 $2^L-1$, 那么 $D^{i-j}$ 就不能被 $g(D)$ 整除; 因此, **所有的两位差错都能够被检出**。

    在实际使用中, 生成多项式 $g(D)$ 常常选用阶为 $L-1$ 的原始多项式和多项式 $D+1$ 的乘积。因为当且仅当 $e(D)$ 包括偶数个非零系数的时候, 多项式 $e(D)$ 才会被 $D+1$ 整除。这确保了**所有的奇数个差错都会被检测到**, 而且原始多项式确保了所有的两个差错都会被检测到(只要所有的帧长度都小于 $2^L-1$)。因此, 所有这种形式的码字都有最小为 4 的最小距离, 一个至少为 L 的突发差错检测能力, 以及在完全随机的串中无法检测出差错的概率 $2^{-L}$。长度 $L=16$ 的标准 CRC 有两个(分别被称为 CRC-16 和 CRC-CCITT)。这些 CRC 的每一个都是 $D+1$ 和 一个原始的 $(L-1)$ 阶多项式, 并因此都具备上述的性质。还有一个长度 $L=32$ 的标准 CRC(即 CRC-32)。他是一个阶为 32 的原始多项式, 且已经被证明, 对长度小于 3007 的帧有最小距离 5, 对长度小于 12,145 的帧有最小距离 4 [FKL86]。

- 如果传输一个分组中途出错却不能被接收端发现, 算作一次分组层误码。该客户使用本次实验描述的信道, 客户的通信系统每天的使用率是 50%, 即：每天只有一半的时间在传输数据, 那么, 根据你对 CRC-32 的检错能力的理解, 发生一次分组层误码事件, 平均需要多少年？

  - 假设当通信系统开启时, 网络层源源不断的产生数据包, 物理层为 8000bps, 点到点传输延迟为 270ms, 需计算 CRC 数据长度为 259 字节, 帧长度 263 字节, 误码率为 $1\mathrm{e}{-5}$。则当系统运行稳定时, 1093.973 秒可收到 4060 个包。

    由于 $263 * 8 = 2104 < 3007$, 故任意两个帧之间有最小距离 5。小于 5 位的错误都可以被检测到,只需要考虑至少有 5 位差错的情形。误码率为 p, 长度为 L 位的帧有至少 5 位差错的概率为

    $$
    p(A) = 1 - \left((1-p)^L+\binom{L}{1}p(1-p)^{L-1}+\binom{L}{2}p^2(1-p)^{L-2}+\binom{L}{3}p^3(1-p)^{L-3}+\binom{L}{4}p^4(1-p)^{L-4}\right)
    \tag{10.1.6}
    $$

    代入数值 $p = 1\mathrm{e}{-5}, L = 2104$, 由于 $\binom{L}{4}p^4(1-p)^{L-4}$ 和其他项都差了至少 $1\mathrm{e}{-5}$ 的数量级, 故忽略此项, 得 $p(A) \approx 8.006\mathrm{e}{-9}$, 进一步计算得在约 143798 万次传输中才会有约 99.999%概率出现至少一次不会被检测出的差错, 即至少有 5 位差错的帧。由于每天只有一半时间传输数据,故计算可得一天可以传输约 160721 个包, 共需要传输约 8947 天, 即平均每 25 年才会出现一次分组层误码事件。

  - 从因特网或其他参考书查找相关资料, 看看 CRC32 有没有充分考虑线路误码的概率模型, 实际校验能力到底怎样。你的推算是过于保守了还是夸大了实际性能？

    - 根据 IEEE 802.3 的[研究][1], 对误码率为 $1\mathrm{e}{-5}$, 长度为 $4096bit$ 的传输环境, 出现无法检出差错的概率小于 $1\mathrm{e}{-15}$, 远小于上面估算的概率, 故我的推算保守了实际性能。 ![crc error detection capability image](imgs/crc32_error_detection_capability.png)

  - 如果你给客户的回答不能让他满意这种分组层误码率, 你还有什么措施降低发生分组层误码事件的概率, 这些措施需要什么代价？

    - 更换校验能力更强的生成多项式, 例如阶为63的原始多项式和 $(D+1)$ 的乘积。代价是物理层涉及校验的部分需要进行修改。

### CRC 校验和的计算方法

- 本次实验中CRC-32校验和的计算直接调用了一个简单的库函数, 8.8节中的库函数 `crc32()` 是从 RFC1663 中复制并修改而来。在 PPP 相关协议文本 RFC1662.TXT (以另外单独一个文件提供) 中含有计算 CRC-32 和 CRC-16 的源代码, 浏览这些源代码。教材中给出了手工计算 CRC 校验和的方法, 通过二进制“模2”除法求余数。这些源代码中却采用了以字节值查表并叠加的方案, 看起来计算速度很快。你能分析出这些算法与我们课后习题中手工进行二进制“模二”除法求余数的算法是等效的吗？

  - 通过查表法逐字节计算CRC与二进制“模2”除法方式在数学上等价。以CRC-16为例推算如下。

    分别用二元多项式 $u(x)$ 和 $s(x)$ 代表收到的信息和对应的校验位。

    $$
    u(x) = u_0+u_1x+u_2x^2+\cdots
    $$ $$
    s(x) = s_0+s_1x+s_2x^2+\cdots+s_{15}x^{15}
    $$
    
    由于 $s(x)$ 是 $x^{16}u(x)$ 除以 $g(x)$ 的余式, 对某个二元多项式 $a(x)$, 有

    $$
    x^{16}u(x) = a(x)g(x) + s(x)
    $$

    现在给收到的信息增加一个比特, 如果从左到右阶从低到高, 则这个操作相当于将原本的信息右移8位后将额外的这个比特放在空出来的低八位上。扩展后的信息 $u'(x)$ 可以被表示为

    $$
    u'(x) = b(x) + x^8u(x), 其中,
    $$

    $$
    b(x) = b_0+b_1x+\cdots+b_7x^7
    $$

    $b(x)$ 即为新增加的字节。将 $u'(x)$ 的校验位表示为 $s'(x)$, 则 $s'(x)$ 即为 $x^{16}u'(x)$ 被 $g(x)$ 除的余式。将其表示为

    $$
    s'(x) = R_{g(x)}[x^{16}u'(x)].
    $$

    用 $u(x)$ 表示 $u'(x)$, 我们有

    $$
    x^{16}u'(x) = x^{16}[b(x)+x^8u(x)]
    $$
    
    $$
    = x^{16}b(x)+x^8[a(x)g(x)+s(x)]
    $$
    
    $$
    = x^{16}b(x)+x^8a(x)g(x)+x^8s(x)
    $$
    
    $$
    = x^16b(x)+x^8a(x)g(x)+x^8s(x)
    $$
    
    又有和的余式等于余式的和, 且 $g(x)$ 整除 $x^8a(x)g(x)$, 故

    $$
    s'(x) = R_{g(x)}[x^{16}u'(x)]
    $$
    
    $$
    = R_{g(x)}[x^{16}b(x)+x^8a(x)g(x)+x^8s(x)]
    $$
    
    $$
    = R_{g(x)}[x^{16}b(x)]+R_{g(x)}[x^8a(x)g(x)]+R_{g(x)}[x^8s(x)]
    $$
    
    $$
    = R_{g(x)}[x^{16}b(x)+x^8s(x)]
    $$
    
    将 $b(x)$ 和 $s(x)$ 展开, 得

    $$
    x^{16}b(x)+x^8s(x)=b_0x^{16}+b_1x^{17}+\cdots+b_7x^{23}
    $$
    
    $$
    +s_0x^8+s_1x^9+\cdots+s_7x^{15}
    $$
    
    $$
    +s_8x^{16}+s_9x^{17}+\cdots+s_{15}x^{23}
    $$
    
    $$
    =(b_0+s_8)x^{16}+(b_1+s_9)x^{17}+\cdots+(b_7+s_{15})x^{23}
    $$
    
    $$
    +s_0x^8+s_1x^9+\cdots+s_7x^{15}
    $$
    
    用 $t_i$ 代替 $[b_i+s_{i+8}], \quad (i = 0, 1, \dots, 7)$, 有

    $$
    x^{16}b(x)+x^8s(x)=t_0x^{16}+t_1x^{17}+\cdots+t_7x^{23}
    $$
    
    $$
    +s_0x^8+s_1x^9+\cdots+s_7x^{15}
    $$

    又由于第二个式子的度小于 $g(x)$, 得

    $$
    s'(x) = R_{g(x)}[t_0x^{16}+t_1x^{17}+\cdots+t_7x^{23}]
    $$
    
    $$
    +R_{g(x)}[s_0x^8+s_1x^9+\cdots+s_7x^{15}]
    $$
    
    $$
    =R_{g(x)}[t_0x^{16}+t_1x^{17}+\cdots+t_7x^{23}]
    $$
    
    $$
    +(s_0x^8+s_1x^9+\cdots+s_7x^{15})
    $$

    这个式子将扩展后的信息的校验位和最初的信息的校验位联系了起来。注意到表达式中 $(s_0x^8+s_1x^9+\cdots+s_7x^{15})$ 表示 $u(x)$ **的校验位中高8位右移8位**。而 $(t_0, t_1, \dots, t_7)$ 只是简单的把 **$(b_0, b_1, \dots, b_7)$ 和 $u(x)$ 的校验位的低8位 $(s_8, s_9, \dots, s_15)$ 模二相加**。据此, 可以得到CRC-16一类度为16的生成多项式对应的**查表算法**。

    对 $(t_0, t_1, \cdots, t_7)$ 的不同取值, 我们可以预先计算出余式 $R_{g(x)}[t_0x^{16}+t_1x^{17}+\cdots+t_7x^{23}]$ 并将其制表。由于 $t_0~t_7$ 一共有8位, 所以这样的表格一共有256项, 每项长2个字节。

    在校验开始前, 先初始化CRC寄存器, 使其存储接受到消息的校验位, 顺序为从左到右由高位到低位。

    1. 初始化 CRC 寄存器为 0x0000, 即将 $s_0~s_15$ 均置0。
    2. 将 $(b_0, b_1, \dots, b_7)$ 和 $(s_8, s_9, \dots, s_{15})$ 进行异或来获得 $(t_0, t_1, \dots, t_7)$。
    3. 将CRC寄存器右移8位。
    4. 查表得对应 $(t_0, t_1, \dots, t_7)$ 的值, 与CRC寄存器做异或操作。
    5. 重复第二到第四步, 直到消息尾, 此时消息的CRC计算完毕, 存储在CRC寄存器中。
    
    对于CRC-32, 也可以用相同的方式进行推导。
    
- 算法中设置的查表数组crc_table[256]是怎样构造出来的？
    - 由上面的推导, 可知crc_table数组中的每一项即 $R_{g(x)}[t_0x^{16}+t_1x^{17}+\cdots+t_7x^{23}]$ 对应不同 $t_0~t_7$ 的值。

    - 你能否写一段c语言程序, 按照模2除法的规则, 用你的程序生成速查表crc_table的256个数字？

    ```c
      uint32_t crc_table[256];

      void GenerateTable() {
        uint32_t polonomial = 0x04C11DB7;
        for (int byte = 0; byte < 256; ++byte) {
          uint32_t crc = byte;
          for (int bit = 32; bit > 0; --bit) {
            if (crc & 0x80000000) {
              crc = (crc << 1) ^ polynomial;
            } else { crc <<= 1; }
          }
          crcTable[byte] = crc;
        }
      }

    ```

### 日志功能

### 可变参数函数

### 定时器函数

### 软件测试

### 流量控制

我们的滑动窗口协议考虑到了两个站点的数据链路层对等实体之间的流量控制问题. 我们通过选择发送窗口和接收窗口的大小以及确认机制来实现这一点. 因为窗口大小有限制, 发送方不会一次性发送过多使接收方被数据所淹没. 另外, 如果接受方无法处理发送过快的帧, 则发送方不会接受到 ACK, 这样也控制了发送流量.

### 协议改进

[1]: https://ieeexplore.ieee.org/document/35380 (T. Fujiwara, T. Kasami and S. Lin, "Error detecting capabilities of the shortened Hamming codes adopted for error detection in IEEE Standard 802.3," in IEEE Transactions on Communications, vol. 37, no. 9, pp. 986-989, Sept. 1989, doi: 10.1109/26.35380.)
[//]: # "$ p(A) \approx 8.00633426223384958575479686260223388671875\mathrm{e}{-9} $"