
// vector:随机访问快
// list:插入开销小
//	list是双向迭代器，不是随机访问迭代器


/*
	定时器链表，按照定时器到期时间升序排列
	
	接口：
		getMinExpired();获得最近的定时器到期时间
		handleExpired();处理超时定时器
		addTimer();添加定时器
		delTimer();删除定时器
*/


#include<list>
#include<time.h>

#include"timer.h"

class TimerList{
public:
	TimerList();
	~TimerList();
	
	time_t getMinExpired();
	void handleExpired(time_t);
	void addTimer(Timer* t);
	void delTimer(Timer* t);
	
	void debugPrint();
	
private:
	std::list<Timer*> timers;
};
