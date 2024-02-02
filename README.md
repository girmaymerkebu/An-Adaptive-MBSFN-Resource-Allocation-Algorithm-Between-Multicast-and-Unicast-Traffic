# An Adaptive MBSFN Resource Allocation Algorithm Between Multicast and Unicast Traffic

## Installation and Execution Procedures

This repository is includes modifications included on top of the srsRAN MBMS solution. The modifications are inluded to enable a periodic SIB generation which is used to adaptiveley set MBSFN 
subframe allocation and periodicity values based on multicast traffic load.  

Procedures to test the Adaptive SF allocation solution:
  * Clone this repository using "git clone [](https://gitlab.ilabt.imec.be/mgirmay/adaptive-mbsfn.git").
  * Install in atleast two host PCs .
  * Run the srsepc, srsmbms, and srsenb (with the enb.conf) in the eNB host PC.  
  * Run srsue (with the configuration file ue.conf) in the UE host PC .
  * Create MBMS gateway 
  * Generate dynamic multicast and unicast traffic load using iperf.
  * Trace the obtained unicast and multicast throughput to observe how the resourse allocation works adaptiveley as the traffic load varies.
### Demo video
Here is a [link](https://www.youtube.com/watch?v=JwHphnfr1xQ) for a demo video of the solution.
## Support
merkebutekaw.girmay@ugent.be 

## Reference
If you utilize the provided source code, please kindly acknowledge and cite the following research paper:

I. Khalid, M. Girmay, V. Maglogiannis, D. Naudts, A. Shahid, and I. Moerman, "[An Adaptive MBSFN Resource Allocation Algorithm for Multicast and Unicast Traffic](https://ieeexplore.ieee.org/document/10060040)," in Proceedings of the 2023 IEEE 20th Consumer Communications & Networking Conference (CCNC), Las Vegas, NV, USA, 2023, pp. 579-586, doi: 10.1109/CCNC51644.2023.10060040.

=======================================================





