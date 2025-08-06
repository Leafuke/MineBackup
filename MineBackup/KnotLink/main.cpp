//#include "SignalSubscriber.hpp"
//#include "SignalSender.hpp"
//#include <chrono>
//#include <thread>

//void onDataReceived(const std::string& data) {
//	std::cout << "Received data:q " << data << std::endl;
//}
//
//int main() {
//	/*SignalSubscriber subscriber("0x00000001", "0x00000020");
//	
//	subscriber.setOnDataReceivedCallback(onDataReceived);*/
//	
//	SignalSender sd("0x00000001", "0x00000020");
//	
//	// Keep the program running
//	while (true) {
//		sd.emitt("2333");
//		std::this_thread::sleep_for(std::chrono::seconds(5));
//	}
//	
//	return 0;
//}

//#include "OpenSocketResponser.hpp"
//#include "OpenSocketQuerier.hpp"
//#include <iostream>
//
//int main() {
//	// 1. 启动回答者
//	OpenSocketResponser res("0x00100000", "0x00000020");
//	res.setQuestionHandler([](const std::string& q){
//		return std::string(q.rbegin(), q.rend());
//	});
//	
//	Sleep(3);
//	
//	// 2. 启动提问者并提问
//	//OpenSocketQuerier cli;
//	//cli.setConfig("0x00100000", "0x00000020");
//	//
//	//std::string ans = cli.query_l("Hello");
//	//std::cout << "Answer: " << ans << std::endl;  // 输出 olleH
//	
//	while(1){ 
//		Sleep(2000);
//		std::cout<<1;
//	}
//	
//	return 0;
//}
