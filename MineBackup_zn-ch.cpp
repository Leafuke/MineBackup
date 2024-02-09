#include <iostream> 
#include <string>
#include <vector>
#include <fstream>
#include <ctime>
#include <thread>
#include <conio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <windows.h> 
using namespace std;
string Gpath,Gpath2[100];
void sprint(string s,int time)//延迟输出 
{
	int len=s.size();
	for(int i=0;i<len;i++)
	{
		if(s[i]=='*')
		{
			Sleep(100);
			continue;
		}
		printf("%c",s[i]);
		Sleep(time);
	}
}

inline void neglect(int x)//读取忽略 x 行 
{
	int num;
	char ch;
	while(num<x)
	{
		ch=getchar();
		if(ch=='\n') ++num;
	}
	/*string tmp;
	int num=0;
	while(num<x)
	{
		getline(cin,tmp);
		++num;
	 } */
}

bool isDirectory(const std::string& path)//寻找文件夹 
{
#ifdef _WIN32
    DWORD attr = GetFileAttributes(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
#else
    DIR* dir = opendir(path.c_str());
    if (dir)
    {
        closedir(dir);
        return true;
    }
    return false;
#endif
}

//列出子文件夹 
void listSubdirectories(const std::string& folderPath, std::vector<std::string>& subdirectories)
{
#ifdef _WIN32
    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA((folderPath + "\\*").c_str(), &findData);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && strcmp(findData.cFileName, ".") != 0 && strcmp(findData.cFileName, "..") != 0)
            {
                subdirectories.push_back(findData.cFileName);
            }
        } while (FindNextFileA(hFind, &findData));
        FindClose(hFind);
    }
#else
    DIR* dir = opendir(folderPath.c_str());
    if (dir)
    {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr)
        {
            if (entry->d_type == DT_DIR && strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
            {
                subdirectories.push_back(entry->d_name);
            }
        }
        closedir(dir);
    }
#endif
}
//获取文件夹最近修改时间 
std::string getModificationDate(const std::string& filePath)//Folder modification date
{
    std::string modificationDate;
#ifdef _WIN32
    WIN32_FIND_DATAA fileData;
    HANDLE hFile = FindFirstFileA(filePath.c_str(), &fileData);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        FILETIME ft = fileData.ftLastWriteTime;
        SYSTEMTIME st;
        FileTimeToSystemTime(&ft, &st);

        char buffer[256];
        snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

        modificationDate = buffer;

        FindClose(hFile);
    }
#else
    struct stat info;
    if (stat(filePath.c_str(), &info) != 0)
        return modificationDate;

    time_t t = info.st_mtime;
    struct tm* tm = localtime(&t);

    char buffer[256];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm);

    modificationDate = buffer;
#endif

    return modificationDate;
}
string temp[100];
//列出文件夹内的文件 
void ListFiles(const std::string& folderPath) {
    std::string searchPath = folderPath + "\\*.*";

    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);
	int i=0;
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            	temp[++i]=findData.cFileName;
                cout<< i << ".  " << findData.cFileName << std::endl;
            }
        } while (FindNextFileA(hFind, &findData));

        FindClose(hFind);
    }
}
//预读取（直到: 
void Qread()
{
	char ch;
	ch=getchar();
	while(ch!=':') ch=getchar();
	return ;
}
//获取注册表的值 
string GetRegistryValue(const std::string& keyPath, const std::string& valueName)
{
    HKEY hKey;
    string valueData;

    if (RegOpenKeyExA(HKEY_CURRENT_USER, keyPath.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
		char buffer[1024];
		DWORD dataSize = sizeof(buffer);
		if (RegGetValueA(hKey, NULL, valueName.c_str(), RRF_RT_ANY, NULL, buffer, &dataSize) == ERROR_SUCCESS) {
			valueData = buffer;
		}
		RegCloseKey(hKey);
	}
	else
	{
		printf("\n\n本程序调用 7-Zip 进行高压缩备份，但你的计算机上尚未安装 7-Zip ，请先到官网 7-zip.org 下载。\n\n");
	}
    return valueData;
}
struct names{
	string real,alias;
	int x;
}name[100];
string rname2[20],Bpath,command,yasuo,lv;//存档真实名 备份文件夹路径 cmd指令 7-Zip路径 压缩等级 
bool prebf,ontop,choice,echos;//回档前备份 工具箱置顶 手动选择 回显 
HWND hwnd;
struct File {
    string name;
    time_t modifiedTime;
};
//存档文件夹路径预处理 
int PreSolve(string s)
{
	int len=s.size(),tt=0;
	for(int i=0;i<len;++i)
	{
		if(s[i]=='$')
		{
			++tt;
			continue;
		}
		Gpath2[tt]+=s[i];
	}
	return tt;
}
//备份函数 
void Backup(int bf,bool echo)
{
	string folderName = Bpath + "/" + name[bf].alias; // Set folder name
	// Create a folder using mkdir ()
	int result = mkdir(folderName.c_str());
	time_t now = time(0);
    tm *ltm = localtime(&now);
    string com=asctime(ltm),tmp="";
    
    for(int j=0;j<com.size();++j)
    	if(j>=11 && j<=18)
    		if(j==13 || j==16) tmp+="-";
    		else tmp+=com[j];
    tmp="["+tmp+"]"+name[bf].alias;
	if(echo) command=yasuo+" a -t7z -mx="+lv+" "+tmp+" \""+name[bf].real+"\"\\*";
	else command=yasuo+" a -t7z -mx="+lv+" "+tmp+" \""+name[bf].real+"\"\\*";
//	cout<< endl << command <<endl;//debug 
	system(command.c_str());
	if(echo) command="move "+tmp+".7z "+folderName;
	else command="move "+tmp+".7z "+folderName;
	system(command.c_str());
	return ;
}
//创建备份文件 
void CreateConfig()
{
	printf("\n你需要创建 (1)一般配置 还是 (2)全自动配置\n\n");
	char ch=getch();
	string folderName,filename = "config1.ini";
	string i="1";
    ifstream file(filename);
    while(true)
    {
    	i[0]+=1;
    	filename="config"+i+".ini";
    	ifstream file(filename);
    	if(!file.is_open()) break;
	}
	if(ch=='1')
	{
		printf("\n正在创建名为 %s 的配置文件\n",&filename[0]);     
    	ofstream newFile(filename);
    	printf("请输入存档文件夹的储存路径 (多个文件夹路径间用$分隔): ");
		getline(cin,Gpath);
		printf("请输入备份存储文件夹路径:");
		getline(cin,Bpath);
		for(int i=0;i<=10;++i)
			Gpath2[i]="";
		int summ=PreSolve(Gpath);
        if (newFile.is_open()) {
        	newFile << "Auto:0" << endl;
        	newFile << "所有存档路径:" << Gpath2[0];
        	if(summ>1) newFile << '$'; 
        	for(int i=1;i<summ;++i)
        		newFile << Gpath2[i] << '$';
        	if(summ!=0) newFile << Gpath2[summ] << endl;
        	else newFile << endl;
            newFile << "备份存储路径:" << Bpath << endl;
			string keyPath = "Software\\7-Zip"; 
			string valueName = "Path";
			string softw=GetRegistryValue(keyPath, valueName),softww="";
			for(int i=0;i<softw.size();++i)
				if(softw[i]==' ') softww+='"',softww+=' ',softww+='"';
				else softww+=softw[i];
            newFile << "压缩软件路径:" << softww+"7z.exe" << endl;
            newFile << "还原前先备份:0" << endl;
            newFile << "工具箱置顶:0" << endl;
            newFile << "手动选择备份:0" << endl;
            newFile << "过程显示:1" << endl;
            newFile << "压缩等级(越高，压缩率越低，但速度越慢):5" << endl;
    	}
    	printf("\n有以下存档:\n\n"); 
    	for(int i=0;i<=summ;++i)
    	{
    		cout << endl; 
    		std::vector<std::string> subdirectories;
			listSubdirectories(Gpath2[i], subdirectories);
		    for (const auto& folderName : subdirectories)
		    {
				std::string NGpath=Gpath2[i]+"/"+folderName;
		        std::string modificationDate = getModificationDate(NGpath);
		        std::cout << "存档名称: " << folderName << endl;
		        std::cout << "最近游玩时间 " << modificationDate << endl;
		        std::cout << "-----------" << endl;
		    }
		    Sleep(2000);
		    sprint("接下来，你需要给这些文件夹起一个易于你自己理解的别名。\n\n",50);
			for (const auto& folderName : subdirectories)
		    {
		        string alias;
		        cout << "请给以下存档设置别名(可以是一段描述) " << folderName << endl;
		        cin >> alias;
				newFile << folderName << endl << alias << endl;
		    }
		    newFile << "$" << endl;
		}
	    newFile << "*" << endl;
	    newFile.close();
	    sprint("配置文件创建完毕！！！\n",10);
        return ;
	}
	else if(ch=='2')
	{
		ofstream newFile(filename);
		newFile << "AUTO:1" << endl;
		int configs;
		printf("需要调用的配置文件序号(从中获取存档名称和别名):\n");
		cin>>configs;
		newFile << "Use Config:" << configs << endl;
		printf("需要备份第几个存档:");
		cin>>configs;
		newFile << "BF:" << configs << endl;
		printf("你需要 (1)定时备份 还是 (2)间隔备份\n");
		ch=getch();
		if(ch=='1'){
			printf("输入你要在什么时间备份: 1.请输入月份，然后回车(输入0表示每个月):");
			int mon,day,hour,min;
			scanf("%d",&mon);
			printf("2.请输入日期，然后回车(输入0表示每天):");
			scanf("%d",&day);
			printf("3.请输入小时，然后回车(输入0表示每小时):");
			scanf("%d",&hour);
			printf("4.请输入分钟，然后回车:");
			scanf("%d",&min);
			newFile << "Mode:1\nTime:" << mon << " " << day << " " << hour << " " << min << endl;
		} 
		else if(ch=='2'){
			printf("输入你要间隔多少分钟备份:");
			int detime;
			scanf("%d",&detime);
			newFile << "Mode:2\nTime:" << detime << endl;
		}
		else{
			printf("\nError\n");
			return ;
		}
		printf("是否开启免打扰模式(0/1):");
		cin>>configs;
		newFile << "Inter:" << configs << "\n*";
		sprint("配置文件创建完毕！！！\n",10);
		return ;
	}
	else
	{
		printf("\n\nError\n\n");
		return ;
	}
}
void Main()
{
	string folderName;
	string filename = "config.ini";
    ifstream file(filename);
    if (!file.is_open()) {
		printf("\n正在建立配置文件......\n"); 
    	ofstream newFile(filename);
    	printf("请输入存档文件夹的储存路径 (多个文件夹路径间用$分隔): ");
		getline(cin,Gpath);
		printf("请输入存档备份存储路径:");
		getline(cin,Bpath);
		int summ=PreSolve(Gpath);
        if (newFile.is_open()) {
        	newFile << "使用的配置文件序号:0" << endl;
        	/*newFile << "Backup parent folder path:" << Gpath2[0] << '$';
        	for(int i=1;i<summ;++i)
        		newFile << Gpath2[i] << '$';
        	newFile << Gpath2[summ] << endl;*/
        	newFile << "存档文件夹路径:" << Gpath << endl;//new
            newFile << "存档备份存储路径:" << Bpath << endl;
			string keyPath = "Software\\7-Zip"; 
			string valueName = "Path";
			string softw=GetRegistryValue(keyPath, valueName),softww="";
			for(int i=0;i<softw.size();++i)
				if(softw[i]==' ') softww+='"',softww+=' ',softww+='"';
				else softww+=softw[i];
            newFile << "压缩软件路径:" << softww+"7z.exe" << endl;
            newFile << "备份前先还原:0" << endl;
            newFile << "工具箱置顶:0" << endl;
            newFile << "手动选择还原(默认选最新):0" << endl;
            newFile << "过程显示:1" << endl;
            newFile << "压缩等级:5" << endl;
    	}
    	printf("\n有以下存档文件夹:\n\n"); 
    	for(int i=0;i<=summ;++i)
    	{
    		cout << endl; 
    		std::vector<std::string> subdirectories;
			listSubdirectories(Gpath2[i], subdirectories);
		    for (const auto& folderName : subdirectories)
		    {
				std::string NGpath=Gpath2[i]+"/"+folderName;
		        std::string modificationDate = getModificationDate(NGpath);
		        std::cout << "存档名称: " << folderName << endl;
		        std::cout << "最近游玩时间: " << modificationDate << endl;
		        std::cout << "-----------" << endl;
		    }
		    Sleep(2000);
		    sprint("下来，你需要给这些文件夹起一个易于你自己理解的别名。\n\n",50);
			for (const auto& folderName : subdirectories)
		    {
		        string alias;
		        cout << "请输入以下存档的别名(可以是一段描述) " << folderName << endl;
		        cin >> alias;
				newFile << folderName << endl << alias << endl;
		    }
		    newFile << "$" << endl;
		}
	    newFile << "*" << endl;
	    newFile.close();
        return ;
    }
    
    
    
    freopen("config.ini","r",stdin);
    Qread();
    string temps,tempss;
    int configs;
    char configss[100];
	scanf("%d",&configs);
	if(configs!=0)
	{
		temps=to_string(configs); //only c++11
		temps="config"+temps+".ini";
		strcpy(configss,temps.c_str());
		freopen(configss,"r",stdin);
		Qread();
		int auto1;
		cin>>auto1;
		if(auto1)
		{
			int mode,usecf,bfnum,ifinter;
			Qread();
			cin>>usecf;
			Qread();
			cin>>bfnum;
			Qread();
			cin>>mode;
			int bftime,month,day,hour,min;
			if(mode==1)
			{
				Qread(); 
				scanf("%d %d %d %d",&month,&day,&hour,&min);//这里如果读入错误，后面就正确…… 
				//2023.12.31解决，配置文件后面多一串东西就行 
			}
			else if(mode==2)
			{
				Qread();
				cin>>bftime;
			}
			Qread();
			cin>>ifinter;
			if(usecf==0) // 使用一般配置中的存档路径 
			{
				freopen("config.ini","r",stdin);
//				getline(cin,tmp1);// Problem Here
				neglect(1); 
				Qread();
				char inputs[1000];
				for(int i=0;;++i)
				{
					inputs[i]=getchar();
					if(inputs[i]=='\n'){
						inputs[i]='\0';break;
					}
				}
				tempss=inputs;
			    int summ=PreSolve(tempss);
			    Qread();
			    memset(inputs,'\0',sizeof(inputs));
			    for(int i=0;;++i)
				{
					inputs[i]=getchar();
					if(inputs[i]=='\n'){
						inputs[i]='\0';break;
					}
				}
				Bpath=inputs;
				Qread();
				memset(inputs,'\0',sizeof(inputs));
			    for(int i=0;;++i)
				{
					inputs[i]=getchar();
					if(inputs[i]=='\n'){
						inputs[i]='\0';break;
					}
				}
				yasuo=inputs;
				neglect(4);
				Qread();
				memset(inputs,'\0',sizeof(inputs));
				inputs[0]=getchar();
				lv=inputs;
			    int i=0;
			    int ttt=0;
			    inputs[0]=getchar();// addition
			    while(true)
			    {
					memset(inputs,'\0',sizeof(inputs));
					for(int i=0;;++i)
					{
						inputs[i]=getchar();
						if(inputs[i]=='\n'){
							inputs[i]='\0';break;
						}
					}
					name[++i].real=inputs;
			    	if(name[i].real[0]=='*') break;
			    	else if(name[i].real[0]=='$')
			    	{
			    		++ttt,--i;
			    		continue;
					}
			    	name[i].real=Gpath2[ttt]+"/"+name[i].real;
			    	name[i].x=ttt;
			    	memset(inputs,'\0',sizeof(inputs));
					for(int i=0;;++i)
					{
						inputs[i]=getchar();
						if(inputs[i]=='\n'){
							inputs[i]='\0';break;
						}
					}
					name[i].alias=inputs;
				}
			}
			else
			{
				string temps=to_string(usecf);
			    char configss[10];
				temps="config"+temps+".ini";
				for(int i=0;i<temps.size();++i)
					configss[i]=temps[i];
				freopen(configss,"r",stdin);
				string tmp;
				getline(cin,tmp);
				Qread();
			    getline(cin,temps);
			    int summ=PreSolve(temps);
			    Qread();
			    getline(cin,Bpath);
			    Qread();
			    getline(cin,yasuo);
			    neglect(4);
				Qread();
				getline(cin,lv);
			    int i=0;
			    int ttt=0;
			    while(true)
			    {
			    	getline(cin,name[++i].real);
			    	if(name[i].real[0]=='*') break;
			    	else if(name[i].real[0]=='$')
			    	{
			    		++ttt,--i;
			    		continue;
					}
			    	name[i].real=Gpath2[ttt]+"/"+name[i].real;
			    	name[i].x=ttt;
			    	getline(cin,name[i].alias);
				}
			}
			if(mode==1)
			{
				while(true)
				{
					// 获取当前时间
				    std::time_t now = std::time(nullptr);
				    std::tm* local_time = std::localtime(&now);
				
				    std::tm target_time = {0};
				    target_time.tm_year = local_time->tm_year; // 年份从1900年开始计算
				    if(month==0)  target_time.tm_mon = local_time->tm_mon;
				    else target_time.tm_mon = month - 1; // 月份从0开始计算
				    if(day==0) target_time.tm_mday = local_time->tm_mday;
				    else target_time.tm_mday = day;
				    if(hour==0) target_time.tm_hour = local_time->tm_hour;
				    else target_time.tm_hour = hour;
				    if(min==0) target_time.tm_min = local_time->tm_min;
				    else target_time.tm_min = min;
				    // 如果当前时间已经超过了目标时间，那么就不需要等待了
				    A: 
				    if (std::mktime(local_time) > std::mktime(&target_time)) {
				        std::cout << "现在的时间已经超过了目标时间" << std::endl;
						if(min!=0 && hour!=0 && day!=0 && month!=0) return ;
						if(min==0)
						{
							++target_time.tm_min;
							if(std::mktime(local_time) <= std::mktime(&target_time)) goto A ;
							--target_time.tm_min;
						}
						if(hour==0)
						{
							++target_time.tm_hour;
							if(std::mktime(local_time) <= std::mktime(&target_time)) goto A ;
							--target_time.tm_hour;
						}
						if(day==0)
						{
							++target_time.tm_mday;
							if(std::mktime(local_time) <= std::mktime(&target_time)) goto A ;
							--target_time.tm_mday;
						}
						if(month==0)
						{
							++target_time.tm_mon;
							if(std::mktime(local_time) <= std::mktime(&target_time)) goto A ;
							--target_time.tm_mon;
						}
						puts("1");
						
				    } else {
				        // 计算需要等待的时间（单位：秒）
				        std::time_t wait_time = std::difftime(std::mktime(&target_time), std::mktime(local_time));
				        // 等待指定的时间
				        std::this_thread::sleep_for(std::chrono::seconds(wait_time));
				        Backup(bfnum,false);
				    }
				}
			} 
			else if(mode==2)
			{
				while(true)
				{
					auto now = std::chrono::steady_clock::now();
				    // 计算目标时间（例如：5秒后）
				    auto target_time = now + std::chrono::seconds(60*bftime);
				
				    // 计算时间差值
				    auto duration = target_time - now;
				
				    // 让线程休眠指定的时间
				    std::this_thread::sleep_for(duration);
				    Backup(bfnum,false);
				}
				
			}
		}
	}
    Qread();
    getline(cin,temps);
    int summ=PreSolve(temps);
    Qread();
    getline(cin,Bpath);
    Qread();
    getline(cin,yasuo);
    Qread();
    cin>>prebf;
    Qread();
    cin>>ontop;
    Qread();
	cin>>choice;
	Qread();
	cin>>echos;
	Qread();
	getline(cin,lv);
    int i=0;
    printf("有以下存档:\n\n");
    int ttt=0;
    //char ch=getchar();//DEBUG because getline ...//bug why?now ok?
    while(true)
    {
    	getline(cin,name[++i].real);
    	if(name[i].real[0]=='*') break;
    	else if(name[i].real[0]=='$')
    	{
    		++ttt,--i;
    		continue;
		}
    	name[i].real=Gpath2[ttt]+"/"+name[i].real;
    	name[i].x=ttt;
    	getline(cin,name[i].alias);
    	printf("%d. ",i);
    	cout<<name[i].alias<<endl;
	}
	freopen("CON","r",stdin);
	
	while(true)
	{
		printf("请问你要 (1)备份存档 (2)回档 (3)更新存档 (4) 自动备份 还是 (5) 创建配置文件 呢？ (按 1/2/3/4/5)\n");
		char ch;
		ch=getch();
		if(ch=='1')
		{
			printf("输入存档前的序号来完成备份:");
			int bf;
			scanf("%d",&bf);
			Backup(bf,echos);
			sprint("\n\n备份完成! ! !\n\n",40);
		}
		else if(ch=='2')
		{
			int i=0;
		    if(!choice) printf("输入存档前的序号来完成还原 (模式: 选取最新备份) : ");
		    else printf("输入存档前的序号来完成还原 (模式: 手动选择) :");
			int bf;
			scanf("%d",&bf);
			string folderPath=Bpath+"/"+name[bf].alias+"/";
			DIR* directory = opendir(folderPath.c_str());
		    if (!directory) {
		        printf("备份不存在，无法还原！你可以还没有进行过备份\n");
		        return ;
		    }
		    File files;
		    if(!choice)//Look for the latest backup
		    {
			    struct dirent* entry;
			    while ((entry = readdir(directory))) {
			        string fileName = entry->d_name;
			        string filePath = folderPath + fileName;
			        struct stat fileStat;
			        if (stat(filePath.c_str(), &fileStat) != -1) {
			            if (S_ISREG(fileStat.st_mode)) { // Only regular files are processed
			                File file;
			                file.name = fileName;
			                file.modifiedTime = fileStat.st_mtime;
							if(file.modifiedTime>files.modifiedTime)
							{
								files.modifiedTime=file.modifiedTime;
								files.name=file.name;
							}
								
			            }
			        }
			    }
			    closedir(directory);
			}
			else
			{
				string folderName2 = Bpath + "/" + name[bf].alias;
				printf("以下是备份存档\n\n");
				ListFiles(folderName2);
				printf("输入备份前的序号来完成还原:");
				int bf2;
				scanf("%d",&bf2);
				files.name=temp[bf2];
			}
		    if(prebf)
		    	Backup(bf,false);
			command=yasuo+" x "+Bpath+"/"+name[bf].alias+"/"+files.name+" -o"+name[bf].real+" -y";
			system(command.c_str());
			sprint("\n\n还原成功! ! !\n\n",40);
		}
		else if(ch=='3')
		{
			freopen("CON","r",stdin);
        	ofstream cfile("config.ini");
        	cfile << "使用的配置文件序号:0\n";
        	cfile << "存档文件夹路径:" << Gpath2[0] << '$';
        	for(int i=1;i<summ;++i)
        		cfile << Gpath2[i] << '$';
        	cfile << Gpath2[summ] << endl;
        	cfile << "存档备份存储路径:" << Bpath << endl;
			string keyPath = "Software\\7-Zip"; 
			string valueName = "Path";
            cfile << "压缩软件路径:" << GetRegistryValue(keyPath, valueName)+"7z.exe" << endl;
            cfile << "备份前还原:" << prebf << endl;
            cfile << "工具箱置顶:" << ontop << endl;
            cfile << "手动选择备份:" << choice << endl;
            cfile << "过程显示:" << echos << endl;
            cfile << "压缩等级:" << lv << endl;
        	printf("\n在文件夹中有以下存档: \n\n"); 
	    	for(int i=0;i<=summ;++i)
	    	{
	    		vector<string> subdirectories;
				listSubdirectories(Gpath2[i], subdirectories);
			    for (const auto& folderName : subdirectories)
			    {
					string NGpath=Gpath2[i]+"/"+folderName;
			        string modificationDate = getModificationDate(NGpath);
			        cout << "存档名称: " << folderName << endl;
			        cout << "最近游玩时间: " << modificationDate << endl;
			        cout << "-----------" << endl;
			    }
			    Sleep(2000);
			    sprint("接下来，你需要给这些文件夹起一个易于你自己理解的别名。\n\n",50);
				for (const auto& folderName : subdirectories)
			    {
			        string alias;
			        cout << "请输入以下存档的别名(可以是一段描述) " << endl << folderName;
			        cin >> alias;
					cfile << folderName << endl << alias << endl;
			    }
			    cfile << "$" << endl;
			}
		    cfile << "*" << endl;
	    	puts("\n\n更新存档完成\n\n");
	    	cfile.close(); 
		}
		else if(ch=='4')
		{
			printf("请输入你要备份的存档序号:");
			int bf,tim;
			scanf("%d",&bf);
			printf("每隔几分钟进行备份: ");
			scanf("%d",&tim);
			printf("已进入自动备份模式，每隔 %d 分钟进行备份",tim);
			while(true)
			{
				Backup(bf,false);
				Sleep(tim*60000);
			}
		}
		else if(ch=='5')
		{
			CreateConfig();
		}
		else printf("请按键盘上的 1/2/3/4/5 键\n\n");
	}
	std::this_thread::sleep_for(std::chrono::seconds(1));
//    return ;
}

bool isRunning = true;
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_COMMAND:
    {
        int buttonId = LOWORD(wParam);

        // Handle button click events
        switch (buttonId)
        {
	        case 1:
        	{
        		int x=0;
	        	while(!Gpath2[x].empty())
	        	{
	        		command="start "+Gpath2[x++];
					system(command.c_str());
				}
	            break;
			}
	        case 2:
	            command="start "+Bpath;
				system(command.c_str());
	            break;
	        case 3:
	            command="start config.ini";
				system(command.c_str());
	            break;
		}
		break;
	}
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	HWND h=GetForegroundWindow();
    WNDCLASS wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "ButtonWindowClass";
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(
        0,                          
        wc.lpszClassName,          
        "Toolbox",                  
        WS_OVERLAPPEDWINDOW,          
        CW_USEDEFAULT, CW_USEDEFAULT, 
        480, 85,              
        NULL,                     
        NULL,                     
        hInstance,
        NULL
    );
    CreateWindow("button", "打开存档文件夹",
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        20, 10, 120, 35,
        hwnd, (HMENU)1, hInstance, NULL);

    CreateWindow("button", "打开备份文件夹",
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        150, 10, 120, 35,
        hwnd, (HMENU)2, hInstance, NULL);

    CreateWindow("button", "设置",
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        280, 10, 80, 35,
        hwnd, (HMENU)3, hInstance, NULL);

    ShowWindow(hwnd, nCmdShow);
    if(ontop) SetWindowPos(h,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE | SWP_NOSIZE);//Top the window
    std::thread MainThread(Main);
    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    isRunning = false;
    MainThread.join();
    return 0;
}
