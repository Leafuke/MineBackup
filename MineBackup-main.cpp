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
string Gpath,Gpath2[100]; //游戏路径，分割游戏路径，对话（多语言）
int lang = 0;
string Dialog[5][100] = {
	{"\n正在建立配置文件......\n",
	"正在尝试寻找已知游戏存档路径...\n",
	"找到官方启动器JAVA版本存档，是否加入路径？(是:1 否:0)\n",
	"未发现JAVA版存档，若存在，请之后手动输入。\n",
	"找到官方启动器基岩版本存档，是否加入路径？(是:1 否:0)\n",
	"未发现基岩版存档。\n",
	"请输入存放存档文件夹的文件夹路径 (写为一行，多个文件夹路径间用$分隔): ",
	"请输入存档备份存储路径:",
	"使用的配置文件序号:0",
	"显示语言:1",
	"存档文件夹路径:",
	"存档备份存储路径:",
	"压缩软件路径:",
	"备份前先还原:0",
	"工具箱置顶:0",
	"手动选择还原(默认选最新):0",
	"过程显示:1",
	"压缩格式:7z",
	"压缩等级:5",
	"保留的备份数量(0表示不限制):0",
	"智能备份:0",
	"配置文件创建完毕！可以关闭程序！\n",
	"\n有以下存档:\n\n",
	"存档名称: ",
	"最近游玩时间: ",
	"你是否希望给所有存档设置别名？(0/1)\n\n",
	"接下来，你需要给这些文件夹起一个易于你自己理解的别名。\n\n",
	"那么将自动以存档文件夹名为别名，如果需要修改别名，请在“设置”中手动修改。\n",
	"请输入以下存档的别名(可以是一段描述) ",
	"文件夹名称不能包含符号 \\  /  :  *  ?  \"  <  >  | ，请重新命名",
	"\n检测到新的存档如下：\n",
	"请问是否更新配置文件？(0/1)\n",
	"\n请手动更新，在对应位置以后添加新存档的“真实名”和“别名”\n",
	"现在的时间已经超过了目标时间",
	"有以下存档:\n\n",
	"请问你要 (1)备份存档 (2)回档 (3)更新存档 (4) 自动备份 还是 (5) 创建配置文件 呢？ (按 1/2/3/4/5)\n",
	"输入存档前的序号来完成备份:",
	"\n\n备份完成! ! !\n\n",
	"输入存档前的序号来完成还原 (模式: 选取最新备份) : ",
	"输入存档前的序号来完成还原 (模式: 手动选择) :",
	"备份不存在，无法还原！你可能还没有进行过备份\n",
	"以下是备份存档\n\n",
	"输入备份前的序号来完成还原:",
	"\n\n还原成功! ! !\n\n",
	"请输入你要备份的存档序号:",
	"每隔几分钟进行备份: ",
	"已进入自动备份模式，备份时间间隔为（单位: 分钟）: ",
	"请按键盘上的 1/2/3/4/5 键\n\n",
	"\n\n本程序调用 7-Zip 进行高压缩备份，但你的计算机上尚未安装 7-Zip ，请先到官网 7-zip.org 下载或下载 MineBackup 的便携版。\n\n",
	"检测到该存档为打开状态，已在该存档下创建临时文件夹。\n\n正在将存档文件夹中所有文件复制到[1临时文件夹]中，然后开始备份\n此过程中请不要随意点击\n",
	"\\1临时文件夹",
	"正在复制...",
	"文件复制完成",
	"\n你需要创建 (1)一般配置 还是 (2)全自动配置\n\n",
	"\n正在创建配置文件，文件名称为",
	"是否为自动配置:",
	"需要调用的配置文件序号(从中获取存档名称和别名):\n",
	"调用的配置文件序号:",
	"需要备份第几个存档:",
	"备份序号:",
	"你需要 (1)定时备份 还是 (2)间隔备份\n",
	"输入你要在什么时间备份: 1.请输入月份，然后回车(输入0表示每个月):",
	"2.请输入日期，然后回车(输入0表示每天):",
	"3.请输入小时，然后回车(输入0表示每小时):",
	"4.请输入分钟，然后回车:",
	"模式:1\n定时（年月日时分）:",
	"输入你要间隔多少分钟备份:",
	"模式:2\n间隔时间:",
	"\n错误\n",
	"是否开启免打扰模式(0/1):",
	"打开存档文件夹",
	"打开备份文件夹",
	"设置"
	},
	{"\nCreating configuration file...\n",
	"Trying to find known game save paths...\n",
	"Found official launcher JAVA edition save, should it be added to the path? (Yes:1 No:0)\n",
	"No JAVA edition saves found, if they exist, please enter manually later.\n",
	"Found official launcher Bedrock edition save, should it be added to the path? (Yes:1 No:0)\n",
	"No Bedrock edition saves found.\n",
	"Please enter the folder path for save folders (write in one line, separate multiple folder paths with $): ",
	"Please enter backup storage path:",
	"Configuration file number being used:0",
	"Display language:1",
	"Save folder path:",
	"Backup storage path:",
	"Compression software path:",
	"Restore before backup:0",
	"Toolbox on top:0",
	"Manual selection for restore (default selects latest):0",
	"Process display:1",
	"Compression format:7z",
	"Compression level:5",
	"Number of backups to keep (0 means unlimited):0",
	"Smart backup:0",
	"Configuration file creation complete! You can close the program!\n",
	"\nHere are the saves:\n\n",
	"Save name: ",
	"Last played time: ",
	"Do you want to set aliases for all saves? (0/1)\n\n",
	"Next, you need to give these folders an alias that is easy for you to understand.\n\n",
	"Then the saves will automatically be named after the save folder names, if you need to modify the alias, please manually edit in 'Settings'.\n",
	"Please enter the alias for the following save (can be a description) ",
	"Folder name cannot contain symbols \\ / : * ? \" < > | , please rename",
	"\nNew saves detected as follows:\n",
	"Do you want to update the configuration file? (0/1)\n",
	"\nPlease manually update, add new save's 'real name' and 'alias' after the corresponding position\n",
	"The current time has exceeded the target time",
	"Here are the saves:\n\n",
	"What would you like to do (1)Backup save (2)Restore (3)Update save (4) Automatic backup or (5) Create configuration file? (Press 1/2/3/4/5)\n",
	"Enter the number before the save to complete the backup:",
	"\n\nBackup complete!!!\n\n",
	"Enter the number before the save to restore (Mode: Select latest backup): ",
	"Enter the number before the save to restore (Mode: Manual selection):",
	"Backup does not exist, unable to restore! You may not have performed a backup yet\n",
	"Here are the backed up saves\n\n",
	"Enter the number before the backup to complete the restore:",
	"\n\nRestore successful!!!\n\n",
	"Please enter the number of the save you want to back up:",
	"Backup every few minutes: ",
	"Automatic backup mode activated, backup interval is (unit: minutes): ",
	"Please press the 1/2/3/4/5 key on your keyboard\n\n",
	"\n\nThis program uses 7-Zip for high compression backup, but 7-Zip is not installed on your computer, please download from the official website 7-zip.org or download the portable version of MineBackup.\n\n",
	"Detected that the save is open, a temporary folder has been created under the save.\n\nCopying all files in the save folder to [1 temporary folder], then starting backup\nDo not click randomly during this process\n",
	"\\1 temporary folder",
	"Copying...",
	"File copy completed",
	"\nDo you need to create (1)General configuration or (2)Fully automatic configuration\n\n",
	"\nCreating configuration file, file name is",
	"Is it an automatic configuration:",
	"Configuration file number to be called (from which to get save names and aliases):\n",
	"Called configuration file number:",
	"Which save needs to be backed up:",
	"Backup number:",
	"Do you need (1)Timed backup or (2)Interval backup\n",
	"Enter when you want to back up: 1. Enter the month, then press enter (enter 0 for every month):",
	"2. Enter the day, then press enter (enter 0 for every day):",
	"3. Enter the hour, then press enter (enter 0 for every hour):",
	"4. Enter the minute, then press enter:",
	"Mode:1\nTimed (year/month/day/hour/minute):",
	"Enter how many minutes you want to back up:",
	"Mode:2\nInterval time:",
	"\nError\n",
	"Enable Do Not Disturb mode (0/1):",
	"Open save folder",
	"Open backup folder",
	"Settings"
	}
};
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
}

bool isDirectory(const std::string& path)//文件夹是否存在 
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
string getModificationDate(const std::string& filePath)//Folder modification date
{
    string modificationDate;
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
            	temp[++i] = findData.cFileName;
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
	if(ch=='#')
		while(ch!='\n')
			ch=getchar();
	while(ch!=':')
	{
		ch=getchar();
		if(ch=='#') //如果是注释行，那么跳过本行 
			while(ch!='\n')
				ch=getchar();	
	}
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
		if(access("7z.exe", F_OK) == 0)
		{
			return "7z.exe"; 
		}
		else
		{
			printf("%s",Dialog[lang][48].c_str());
			system("del config.ini");
			system("pause");
			exit(0);
		}
	}
    return valueData;
}
struct names{
	string real,alias;
	int x;
}name[100];
string rname2[30],Bpath,command,yasuo,lv,format;//存档真实名 备份文件夹路径 cmd指令 7-Zip路径 压缩等级 
bool prebf,ontop,choice,echos,smart;//回档前备份 工具箱置顶 手动选择 回显 智能备份 
int limitnum;
HWND hwnd;
struct File {
    string name;
    time_t modifiedTime;
};
//判断文件是否被占用 
bool isFileLocked(const string& filePath)
{
    HANDLE hFile = CreateFile(filePath.c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_SHARE_READ, NULL);
    
    if (hFile == INVALID_HANDLE_VALUE)
    {
        DWORD error = GetLastError();
        // 如果文件正被另一个进程以独占方式打开，则会返回ERROR_SHARING_VIOLATION错误。
        if (error == ERROR_SHARING_VIOLATION)
            return true;
        // 其他错误情况需要根据实际情况处理
        else
            return false;
    }
    else
    {
        CloseHandle(hFile);
        // 文件成功打开且关闭，表明当前没有被占用（至少在这个瞬间）
        return false;
    }
}

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
//检验别名合理性
bool checkupName(string Name)
{
	int len=Name.size();
	for(int i=0;i<len;++i)
		if(Name[i]=='\\' || Name[i]=='/' || Name[i]==':' || Name[i]=='*' || Name[i]=='?' || Name[i]=='"' || Name[i]=='<' || Name[i]=='>' || Name[i]=='|')
			return false;
	return true;
}
//检测备份数量
void checkup(string folderPath,int limit)
{
	if(limit==0) return ;
	DIR* directory = opendir(folderPath.c_str());
    if (!directory) return ;
    File files;
    struct dirent* entry;
    int checknum=0;
    while ((entry = readdir(directory))) {
    	string fileName = entry->d_name;
		string filePath = folderPath + fileName;
		struct stat fileStat;
		stat(filePath.c_str(), &fileStat);
    	if (S_ISREG(fileStat.st_mode)) {
    		++checknum;//如果是常规文件，统计总备份数 
    	}
    }
    closedir(directory);
    struct dirent* entry2;
    while (checknum > limit)
    {
    	directory = opendir(folderPath.c_str());//放外面会造成读取重复，只会删除一次，后面都找不到 
		bool fl=0;
		while ((entry2 = readdir(directory))) {
		    string fileName = entry2->d_name;
		    string filePath = folderPath + fileName;
		    struct stat fileStat;
		    if(!fl) files.modifiedTime=fileStat.st_mtime,fl=1; //重置files 
		    if (stat(filePath.c_str(), &fileStat) != -1) {
		    	if (S_ISREG(fileStat.st_mode)) {
			        File file;
			        file.modifiedTime = fileStat.st_mtime;
					if(file.modifiedTime <= files.modifiedTime)
					{
						files.modifiedTime=file.modifiedTime;
						files.name=fileName;
					}
				}
		    }
		}
		string command="del \"" + folderPath + files.name + "\"";
		system(command.c_str());
		--checknum;
		closedir(directory);
	}
	return ;
}
//备份函数 
void Backup(int bf,bool echo)
{
	string folderName = Bpath + "\\" + name[bf].alias; // Set folder name
	// Create a folder using mkdir ()
	mkdir(folderName.c_str());
	
	bool isFileLock = 0;
	if(isFileLocked(name[bf].real+"\\region\\r.0.0.mca"))
	{
		isFileLock = true; 
		printf("%s",Dialog[lang][49].c_str());
		command = "start \"\"  \"" + name[bf].real + "\"";//这样打开不会报错 
		system(command.c_str());
		Sleep(2000);
	    keybd_event(0x11, 0, 0, 0);//Ctrl
		keybd_event(0x41, 0, 0, 0);//A
		keybd_event(0x41, 0, KEYEVENTF_KEYUP, 0);
		keybd_event(0x43, 0, 0, 0);//C
		keybd_event(0x43, 0, KEYEVENTF_KEYUP, 0);
		keybd_event(0x11, 0, KEYEVENTF_KEYUP, 0);
		Sleep(500);
		string folderName2 = name[bf].real + Dialog[lang][50];
		mkdir(folderName2.c_str());
		command = "start \"\"  \"" + folderName2 + "\"";
		system(command.c_str());
		Sleep(500);
		keybd_event(0x11, 0, 0, 0);//Ctrl
		keybd_event(0x56, 0, 0, 0);//V
		keybd_event(0x56, 0, KEYEVENTF_KEYUP, 0);//V 
		keybd_event(0x11, 0, KEYEVENTF_KEYUP, 0);//Ctrl
		Sleep(2000);
		keybd_event(0x12, 0, 0, 0);//Alt 
		keybd_event(0x53, 0, 0, 0);//Skip
		keybd_event(0x53, 0, KEYEVENTF_KEYUP, 0);
		keybd_event(0x12, 0, KEYEVENTF_KEYUP, 0);
	    // 获取复制进度窗口的句柄
//	    Sleep(10);
	    HWND hForegroundWindow = GetForegroundWindow();
	    cout << Dialog[lang][51] << endl;
	    // 循环检查窗口是否还存在
	    int sumtime=0;
	    while (true) {
	        // 等待一段时间再检查，避免高CPU占用
	        Sleep(2000); // 等待2秒
	        sumtime+=2;
	        // 检查窗口是否仍然有效 或者 复制太快了根本没抓到窗口 
	        if (!IsWindow(hForegroundWindow) || sumtime > 10)
	        {
	        	cout << Dialog[lang][52] << endl;
	        	break; // 窗口关闭，退出循环
			}
	    }
		Sleep(1000); 
		keybd_event(0x01, 0, 0, 0);//左键 
		keybd_event(0x01, 0, KEYEVENTF_KEYUP, 0);
		keybd_event(0x12, 0, 0, 0);//Alt 
		keybd_event(0x73, 0, 0, 0);//F4
		Sleep(100);
		keybd_event(0x01, 0, 0, 0);//左键 
		keybd_event(0x01, 0, KEYEVENTF_KEYUP, 0);
		keybd_event(0x73, 0, 0, 0);//F4
		keybd_event(0x73, 0, KEYEVENTF_KEYUP, 0);
		keybd_event(0x12, 0, KEYEVENTF_KEYUP, 0);
	}
	else //记录一下备份时区块修改时间，方便以后快速压缩的构建 
	{
		//现在在QuickBackup中才记录 
	}
	
	time_t now = time(0);
    tm *ltm = localtime(&now);
    string com=asctime(ltm),tmp="";
    
    for(int j=0;j<com.size();++j)
    	if(j>=11 && j<=18)
    		if(j==13 || j==16) tmp+="-";
    		else tmp+=com[j];
    tmp="["+tmp+"]"+name[bf].alias;
    
    string Real = name[bf].real;
    if(isFileLock)
    	Real+=Dialog[lang][50]; 
	if(echo) command=yasuo+" a -t"+format+" -mx="+lv+" "+tmp+" \""+Real+"\"\\*";
	else command=yasuo+" a -t"+format+" -mx="+lv+" "+tmp+" \""+Real+"\"\\* > nul 2>&1";
//	cout<< endl << command <<endl;//debug 
	system(command.c_str());
	if(echo) command="move "+tmp+".7z "+folderName;
	else command="move "+tmp+".7z "+folderName+" > nul 2>&1";
	system(command.c_str());
	checkup(folderName+"\\",limitnum);
	
	if(isFileLock)
	{
		command = "rmdir /S /Q \"" + Real + "\"";
		system(command.c_str());
	}
//	freopen("CON","w",stdout);
	return ;
}

//智能备份（快速备份） 
void QuickBackup(int bf, bool echo)
{
	string folderName = Bpath + "\\" + name[bf].alias; // Set folder name
	mkdir(folderName.c_str());
	
	//还没有记录过，记录一下备份时区块修改时间
	struct stat fileStat;
	string time1 = name[bf].real + "\\region\\r.0.0.mca", addition;
	if(stat(time1.c_str(), &fileStat))//如果不存在mca文件，那么是基岩版存档，区别仅在于是在region下还是在db下 
	{
		addition = "\\db";
	}
	else
	{
		addition = "\\region";
	}
	string accesss = name[bf].real + "\\Time.txt"; 
	struct dirent* entry;
	time1 = name[bf].real + addition;
	DIR* directory3 = opendir(time1.c_str());
	bool ifbackup = 0;
	if(access(accesss.c_str(), F_OK) != 0) // 如果没有Time.txt，则先记录，然后备份一次 
	{
		ifbackup = true;
		time1 = name[bf].real + "\\Time.txt";
		ofstream newFile(time1);
		time1 = name[bf].real + addition;
	    while ((entry = readdir(directory3))) {
	        string fileName = entry->d_name;
	        string filePath = time1 + "\\" + fileName;
	        if (stat(filePath.c_str(), &fileStat) != -1) {
	            if (S_ISREG(fileStat.st_mode)) { // Only regular files are processed
					newFile << fileName << " " << fileStat.st_mtime << endl;
	            }
	        }
	    }
	    newFile << "* -1" << endl ;
	    closedir(directory3);	
	}
	
    time_t now = time(0);
	tm *ltm = localtime(&now);
	string com=asctime(ltm),tmp="";
    for(int j=0;j<com.size();++j) //初步处理文件名 
    	if(j>=11 && j<=18)
    		if(j==13 || j==16) tmp+="-";
    		else tmp+=com[j];
    tmp="Q["+tmp+"]"+name[bf].alias; //快速备份文件名多个Q 
    
    accesss = name[bf].real + "\\Time.txt"; 
//	freopen(accesss.c_str(),"r",stdin);
	ifstream usage;
	usage.open(accesss.c_str());
	string mca[500],number,backlist="";
	long long moditime[500],ii=0;
	while(moditime[ii] != -1) //从Time.txt中读取时间 
	{
		getline(usage,mca[++ii],' ');
		getline(usage,number,'\n');
		moditime[ii] = stoi(number); //将number转为整型 
	}
		
	
	freopen("CON","r",stdin);
	time1 = name[bf].real + addition;
	directory3 = opendir(time1.c_str()); //重新open，避免第一次bug 
    while ((entry = readdir(directory3))) {// 将新的mca文件备份
        string fileName = entry->d_name;
        string filePath = time1 + "\\" + fileName;
        if (stat(filePath.c_str(), &fileStat) != -1) {
            if (S_ISREG(fileStat.st_mode)) { // Only regular files are processed
				bool mcaok = false;
				for(int i = 1 ; i <= ii; ++i)
				{
					if(fileName == mca[i])
					{
						mcaok = true; 
						if(fileStat.st_mtime == moditime[i])
							mcaok = false;
					}
				}
				if(mcaok || ifbackup)
				{
					string Real = name[bf].real + addition +"\\" + fileName;
					backlist = backlist + " \"" + Real + "\"";
				}
            }
        }
    }
    if(backlist.size() <= 5)//没有任何改动 
    {
    	puts("No Edition");
    	return ; 
	}
		
    if(echo) command=yasuo+" a -t"+format+" -mx="+lv+" "+tmp+backlist;
	else command=yasuo+" a -t"+format+" -mx="+lv+" "+tmp+backlist+" > nul 2>&1";
	system(command.c_str());
	if(echo) command="move "+tmp+".7z "+folderName;
	else command="move "+tmp+".7z "+folderName+" > nul 2>&1";
	system(command.c_str());
	checkup(folderName+"\\",limitnum);
	freopen("CON","w",stdout);
	
	
	directory3 = opendir(time1.c_str());
	//再次记录备份时间 
	time1 = name[bf].real + "\\Time.txt";
	ofstream newFile(time1);
	time1 = name[bf].real + addition;
    while ((entry = readdir(directory3))) {
        string fileName = entry->d_name;
        string filePath = time1 + "\\" + fileName;
        if (stat(filePath.c_str(), &fileStat) != -1) {
            if (S_ISREG(fileStat.st_mode)) { // Only regular files are processed
				newFile << fileName << " " << fileStat.st_mtime << endl;
            }
        }
    }
    newFile << "* -1" << endl ;
    closedir(directory3);
	
	return ;
	
}

void Qrestore(int bf,int rs, bool echo)
{
	struct stat fileStat;
	string time1 = name[bf].real + "\\region\\r.0.0.mca", addition;
	if(stat(time1.c_str(), &fileStat))//如果不存在mca文件，那么是基岩版存档，区别仅在于是在region下还是在db下 
	{
		addition = "\\db";
	}
	else
	{
		addition = "\\region";
	}
	string folderPath = Bpath + "\\" + name[bf].alias + "\\";
	DIR* directory = opendir(folderPath.c_str());
	bool st[100]; //用于记录是否已经还原过
	memset(st,0,sizeof(st)); 
    File files;
    while(files.name.compare(temp[rs]))//寻找比目标更 新 的备份，一个个还原 直到目标备份 compare返回值为0表示二者相等 
    {
    	int tt = 0, ttt;
	    struct dirent* entry;
	    directory = opendir(folderPath.c_str());//必须重新打开 
	    while ((entry = readdir(directory))) {
	        ++tt;
	        if(st[tt]) continue;	        
			string fileName = entry->d_name;
	        string filePath = folderPath + fileName;
	        struct stat fileStat;
	        if (stat(filePath.c_str(), &fileStat) != -1) {
	            if (S_ISREG(fileStat.st_mode)) { // Only regular files are processed
					File file;
	                file.name = fileName;
	                file.modifiedTime = fileStat.st_mtime;
					if(file.modifiedTime >= files.modifiedTime)
					{
						files.modifiedTime = file.modifiedTime;
						files.name = file.name;
						ttt = tt;
					}
	            }
	        }
	    }

		command=yasuo+" x "+Bpath+"\\"+name[bf].alias+"\\"+files.name+" -o"+name[bf].real+addition+" -y";
		system(command.c_str());
	    files.modifiedTime = 0;//重置！ 
	    st[ttt] = true;
	    closedir(directory);
	}
    
	sprint(Dialog[lang][43],40);
}

//巧妙地返回环境变量 (现已弃用，直接使用getenv)
/*string GetSpPath(string sp)
{
	freopen("temp","w",stdout);
	system(sp.c_str());
	ifstream tmp("temp");
	freopen("CON","w",stdout);
	string ans;
	getline(tmp, ans, '\n');
//    system("del temp"); 这样会导致freopen出错 
	return ans;
}*/

//初始设置/更新设置 
void SetConfig(string filename, bool ifreset, int summ)
{
	//现在将创建配置文件整合为一个函数 SetConfig() 
	freopen("CON","r",stdin);
	cin.clear();
	printf("%s",Dialog[lang][0].c_str()); 
	ofstream newFile(filename);
	if(ifreset)
	{
		printf("%s",Dialog[lang][1].c_str());
		string searchPath = "", searchTemp = "C:\\Users\\" + (string)getenv("USERNAME") + "\\Appdata\\Roaming\\.minecraft\\saves";
		if(isDirectory(searchTemp)) //对于%Appdata%这样的环境变量，需要特殊处理 GetSpPath("echo %Appdata%") 现在可以直接getenv 
		{
			printf("%s",Dialog[lang][2].c_str());
			char ifadd = getch();
			if(ifadd == '1')
				searchPath += searchTemp + "$";
		}
		else
			printf("%s",Dialog[lang][3].c_str());//GetSpPath("echo %LOCALAPPDATA%")
		searchTemp = "C:\\Users\\" + (string)getenv("USERNAME") + "\\Appdata\\Local\\Packages\\Microsoft.MinecraftUWP_8wekyb3d8bbwe\\LocalState\\games\\com.mojang\\minecraftWorlds";
		if(isDirectory(searchTemp)) 
		{
			printf("%s",Dialog[lang][4].c_str());
			char ifadd = getch();
			if(ifadd == '1')
				searchPath += searchTemp + "$";
		}
		else
			printf("%s",Dialog[lang][5].c_str());
		printf("%s",Dialog[lang][6].c_str());
		getline(cin,Gpath);
		if(Gpath == "") Gpath = searchPath, Gpath.pop_back(); //删除最后的"$" 
		else Gpath = searchPath + Gpath;
		printf("%s",Dialog[lang][7].c_str());
		getline(cin,Bpath);
		summ=PreSolve(Gpath);
//		freopen("CON","w",stdout); 
//		system("del temp"); //一旦删除就会出问题 
	}
	else
	{
		Gpath="";
		for(int i=0;i<summ;++i)
        	Gpath+=Gpath2[i],Gpath+="$";
        Gpath+=Gpath2[summ];
	}	
	
    if (newFile.is_open()) {
    	newFile << Dialog[lang][8] << endl; //位置9留给语言设置选项 
    	newFile << Dialog[lang][10] << Gpath << endl;//new
        newFile << Dialog[lang][11] << Bpath << endl;
		string keyPath = "Software\\7-Zip"; 
		string valueName = "Path";
		string softw=GetRegistryValue(keyPath, valueName),softww="";
		for(int i=0;i<softw.size();++i)
			if(softw[i]==' ') softww+='"',softww+=' ',softww+='"';
			else softww+=softw[i];
        newFile << Dialog[lang][12] << softww+"7z.exe" << endl;
        newFile << Dialog[lang][13] << endl;
        newFile << Dialog[lang][14] << endl;
        newFile << Dialog[lang][15] << endl;
        newFile << Dialog[lang][16] << endl;
        newFile << Dialog[lang][17] << endl;
        newFile << Dialog[lang][18] << endl;
        newFile << Dialog[lang][19] << endl;
        newFile << Dialog[lang][20] << endl;
	}
	printf("%s",Dialog[lang][22].c_str()); 
	for(int i=0;i<=summ;++i)
	{
		bool ifalias=true; // 是否手动设置别名 
		cout << endl; 
		std::vector<std::string> subdirectories;
		listSubdirectories(Gpath2[i], subdirectories);
	    for (const auto& folderName : subdirectories)
	    {
			std::string NGpath = Gpath2[i] + "\\" + folderName;
	        std::string modificationDate = getModificationDate(NGpath);
	        std::cout << Dialog[lang][23] << folderName << endl;
	        std::cout << Dialog[lang][24] << modificationDate << endl;
	        std::cout << "-----------" << endl;
	    }
	    Sleep(1000);
	    sprint(Dialog[lang][25],30);
	    cin>>ifalias; 
	    if(ifalias) sprint(Dialog[lang][26],30);
		else sprint(Dialog[lang][27],30);
		for (const auto& folderName : subdirectories)
	    {
	        string alias;
	        B:
	        if(ifalias)
			{
				cout << Dialog[lang][28] << folderName << endl;
	        	cin >> alias;
			}
			else alias = folderName;
	        if(!checkupName(alias))
			{
				printf("%s",Dialog[lang][29].c_str());
				goto B;
			}
			newFile << folderName << endl << alias << endl;
	    }
	    newFile << "$" << endl;
	}
    newFile << "*" << endl;
    newFile.close();
    printf("%s",Dialog[lang][21].c_str());
    return ;
}

//创建新的备份文件 
void CreateConfig()
{
	printf("%s",Dialog[lang][53].c_str());
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
		printf("%s %s\n",Dialog[lang][54].c_str(),filename.c_str());
    	ofstream newFile(filename);
    	printf("%s",Dialog[lang][6].c_str());
		getline(cin,Gpath);
		printf("%s",Dialog[lang][7].c_str());
		getline(cin,Bpath);
		for(int i=0;i<=10;++i)
			Gpath2[i]="";
		int summ=PreSolve(Gpath);
        if (newFile.is_open()) {
        	newFile << Dialog[lang][55] << "0" << endl;
        	newFile << Dialog[lang][10] << Gpath2[0];
        	if(summ>1) newFile << '$'; 
        	for(int i=1;i<summ;++i)
        		newFile << Gpath2[i] << '$';
        	if(summ!=0) newFile << Gpath2[summ] << endl;
        	else newFile << endl;
            newFile << Dialog[lang][7] << Bpath << endl;
			string keyPath = "Software\\7-Zip"; 
			string valueName = "Path";
			string softw=GetRegistryValue(keyPath, valueName),softww="";
			for(int i=0;i<softw.size();++i)
				if(softw[i]==' ') softww+='"',softww+=' ',softww+='"';
				else softww+=softw[i];
            newFile << Dialog[lang][12] << softww+"7z.exe" << endl;
            newFile << Dialog[lang][13] << endl;
            newFile << Dialog[lang][14] << endl;
            newFile << Dialog[lang][15] << endl;
            newFile << Dialog[lang][16] << endl;
            newFile << Dialog[lang][17] << endl;
	        newFile << Dialog[lang][18] << endl;
	        newFile << Dialog[lang][19] << endl;
	        newFile << Dialog[lang][20] << endl;
    	}
    	printf("%s",Dialog[lang][22].c_str()); 
    	for(int i=0;i<=summ;++i)
    	{
    		cout << endl; 
    		std::vector<std::string> subdirectories;
			listSubdirectories(Gpath2[i], subdirectories);
		    for (const auto& folderName : subdirectories)
		    {
				std::string NGpath=Gpath2[i]+"\\"+folderName;
		        std::string modificationDate = getModificationDate(NGpath);
		        std::cout << Dialog[lang][23] << folderName << endl;
		        std::cout << Dialog[lang][24] << modificationDate << endl;
		        std::cout << "-----------" << endl;
		    }
		    Sleep(2000);
		    sprint(Dialog[lang][26],50);
			for (const auto& folderName : subdirectories)
		    {
		        string alias;
		        cout << Dialog[lang][28] << folderName << endl;
		        cin >> alias;
				newFile << folderName << endl << alias << endl;
		    }
		    newFile << "$" << endl;
		}
	    newFile << "*" << endl;
	    newFile.close();
	    sprint(Dialog[lang][21],10);
        return ;
	}
	else if(ch=='2')
	{
		ofstream newFile(filename);
        newFile << Dialog[lang][55] << "1" << endl;
		int configs;
		printf("%s",Dialog[lang][56].c_str());
		cin>>configs;
		newFile << Dialog[lang][57] << configs << endl;
		printf("%s",Dialog[lang][58].c_str());
		cin>>configs;
		newFile << Dialog[lang][59] << configs << endl;
		printf("%s",Dialog[lang][60].c_str());
		ch=getch();
		if(ch=='1'){
			printf("%s",Dialog[lang][61].c_str());
			int mon,day,hour,min;
			scanf("%d",&mon);
			printf("%s",Dialog[lang][62].c_str());
			scanf("%d",&day);
			printf("%s",Dialog[lang][63].c_str());
			scanf("%d",&hour);
			printf("%s",Dialog[lang][64].c_str());
			scanf("%d",&min);
			newFile << Dialog[lang][65] << mon << " " << day << " " << hour << " " << min << endl;
		} 
		else if(ch=='2'){
			printf("%s",Dialog[lang][66].c_str());
			int detime;
			scanf("%d",&detime);
			newFile << Dialog[lang][67] << detime << endl;
		}
		else{
			printf("%s",Dialog[lang][68].c_str());
			return ;
		}
		printf("%s",Dialog[lang][69].c_str());
		cin>>configs;
		newFile << "Inter:" << configs << "\n*";
		sprint(Dialog[lang][21],10);
		return ;
	}
	else
	{
		printf("%s",Dialog[lang][68].c_str());
		return ;
	}
}
void Main()
{
	LANGID lang_id = GetUserDefaultUILanguage();
	if(lang_id == 2052 || lang_id == 1028) //设置成中文 
		lang = 0;
	else lang = 1; 
	string folderName;
	string filename = "config.ini";
    ifstream file(filename);
    if (!file.is_open()) {
    	//现在将创建配置文件整合为一个函数 SetConfig() 
		SetConfig(filename, true, 0);
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
					inputs[i] = getchar();
					if(inputs[i] == '\n'){
						inputs[i] = '\0';break;
					}
				}
				tempss=inputs;
			    int summ=PreSolve(tempss);
			    Qread();
			    memset(inputs,'\0',sizeof(inputs));
			    for(int i=0;;++i)
				{
					inputs[i] = getchar();
					if(inputs[i] == '\n'){
						inputs[i] = '\0';break;
					}
				}
				Bpath=inputs;
				Qread();
				memset(inputs,'\0',sizeof(inputs));
			    for(int i=0;;++i)
				{
					inputs[i] = getchar();
					if(inputs[i] == '\n'){
						inputs[i] = '\0';break;
					}
				}
				yasuo = inputs;
				neglect(4);
				Qread();
				memset(inputs,'\0',sizeof(inputs));
				inputs[0] = getchar(),inputs[1] = getchar();
				format = inputs;
				Qread();
				memset(inputs,'\0',sizeof(inputs));
				inputs[0] = getchar();
				lv = inputs;
				Qread();
				cin >> smart;
				Qread();
				cin >> limitnum;
			    int i = 0,ttt = 0;//存档数量 存档所在存档文件夹序号 
			    inputs[0] = getchar();// addition
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
			    	name[i].real=Gpath2[ttt]+"\\"+name[i].real;
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
				//检测存档文件夹是否有更新 
				int ix=0;
				bool ifnew=0; 
				for(int i=0;i<=summ;++i)
				{
					std::vector<std::string> subdirectories;
					listSubdirectories(Gpath2[i], subdirectories);
				    for (const auto& folderName : subdirectories)
				    {
				    	if((Gpath2[i]+"/"+folderName)==name[++ix].real) //与实际一致则不更新 注意，这里的检测是有缺陷的 
				    		continue;
						--ix;//因为多出一个，所以比对时-1，但未能处理减少情况 
				    	//不一致，则列出
				    	ifnew=true;
						printf("%s",Dialog[lang][30].c_str());
						string NGpath=Gpath2[i]+"/"+folderName;
				        string modificationDate = getModificationDate(NGpath);
				        cout << Dialog[lang][23] << folderName << endl;
				        cout << Dialog[lang][24] << modificationDate << endl;
				        cout << "-----------" << endl;
				    }
				}
				if(ifnew) //如果有更新，询问是否更新配置文件
				{
					printf("%s",Dialog[lang][31].c_str());
					char ch;
					ch=getch();
					printf("%s",Dialog[lang][32].c_str());
					if(ch=='1')
						system("start config.ini");
				}
			}
			else
			{
				string temps=to_string(usecf);
			    char configss[10];
				temps="config"+temps+".ini";
				for(int i=0;i<temps.size();++i)
					configss[i] = temps[i];
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
				getline(cin,format);
				Qread();
				getline(cin,lv);
				Qread();
				cin>>limitnum;
				Qread();
				cin>>smart;
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
				//检测存档文件夹是否有更新 
				int ix=0;
				bool ifnew=0; 
				for(int i=0;i<=summ;++i)
				{
					std::vector<std::string> subdirectories;
					listSubdirectories(Gpath2[i], subdirectories);
				    for (const auto& folderName : subdirectories)
				    {
				    	if((Gpath2[i]+"/"+folderName)==name[++ix].real) //与实际一致则不更新 注意，这里的检测是有缺陷的 
				    		continue;
						--ix;//因为多出一个，所以比对时-1，但未能处理减少情况 
				    	//不一致，则列出
				    	ifnew=true;
						printf("%s",Dialog[lang][30].c_str());
						string NGpath=Gpath2[i]+"\\"+folderName;
				        string modificationDate = getModificationDate(NGpath);
				        cout << Dialog[lang][23] << folderName << endl;
				        cout << Dialog[lang][24] << modificationDate << endl;
				        cout << "-----------" << endl;
				    }
				}
				if(ifnew) //如果有更新，询问是否更新配置文件
				{
					printf("%s",Dialog[lang][31].c_str());
					char ch;
					ch=getch();
					printf("%s",Dialog[lang][32].c_str()); 
					string command="start "+temps;
					if(ch=='1')
						system(command.c_str());
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
				        std::cout << Dialog[lang][33] << std::endl;
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
				    } else {
				        // 计算需要等待的时间（单位：秒）
				        std::time_t wait_time = std::difftime(std::mktime(&target_time), std::mktime(local_time));
				        // 等待指定的时间
				        std::this_thread::sleep_for(std::chrono::seconds(wait_time));
				        if(!smart) Backup(bfnum,false);
				        else QuickBackup(bfnum,false);
				    }
				}
			} 
			else if(mode==2)
			{
				while(true)
				{
				    // 让线程休眠指定的时间
				    std::this_thread::sleep_for(std::chrono::seconds(60*bftime));
				    if(!smart) Backup(bfnum,false);
				    else QuickBackup(bfnum,false);
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
	getline(cin,format);
	Qread();
	getline(cin,lv);
	Qread();
	cin>>limitnum;
	Qread();
	cin>>smart;
    int i=0,ttt=0;//存档数量 存档所在存档文件夹序号 
    printf("%s",Dialog[lang][34].c_str());
    char ch=getchar();//DEBUG because getline ...//bug why?now ok?
    while(true)
    {
    	getline(cin,name[++i].real);
    	if(name[i].real[0]=='*') break;
    	else if(name[i].real[0]=='$')
    	{
    		++ttt,--i;
    		continue;
		}
    	name[i].real=Gpath2[ttt]+"\\"+name[i].real;
    	name[i].x=ttt;
    	getline(cin,name[i].alias);
    	printf("%d. ",i);
    	cout<<name[i].alias<<endl;
	}
	freopen("CON","r",stdin);
	//检测存档文件夹是否有更新 
	int ix=0;
	bool ifnew=0; 
	for(int i=0;i<=summ;++i)
	{
		std::vector<std::string> subdirectories;
		listSubdirectories(Gpath2[i], subdirectories);
	    for (const auto& folderName : subdirectories)
	    {
	    	if((Gpath2[i]+"\\"+folderName)==name[++ix].real) //与实际一致则不更新 注意，这里的检测是有缺陷的 
	    		continue;
			--ix;//因为多出一个，所以比对时-1，但未能处理减少情况 
	    	//不一致，则列出
	    	ifnew=true;
			printf("%s",Dialog[lang][30].c_str());
			string NGpath=Gpath2[i]+"\\"+folderName;
	        string modificationDate = getModificationDate(NGpath);
	        cout << Dialog[lang][23] << folderName << endl;
	        cout << Dialog[lang][24] << modificationDate << endl;
	        cout << "-----------" << endl;
	    }
	}
	if(ifnew) //如果有更新，询问是否更新配置文件
	{
		printf("%s",Dialog[lang][31].c_str());
		char ch;
		ch=getch();
		printf("%s",Dialog[lang][32].c_str());
		if(ch=='1')
			system("start config.ini");
	}
	while(true)
	{
		printf("%s",Dialog[lang][35].c_str());
		char ch;
		ch=getch();
		if(ch=='1')
		{
			printf("%s",Dialog[lang][36].c_str());
			int bf;
			scanf("%d",&bf);
			if(!smart) Backup(bf,echos);
			else QuickBackup(bf,echos);
			sprint(Dialog[lang][37],40);
		}
		else if(ch=='2') //还原存档 
		{
			int i=0;
		    if(!choice) printf("%s",Dialog[lang][38].c_str());
		    else printf("%s",Dialog[lang][39].c_str());
			int bf;
			scanf("%d",&bf);
			string folderPath = Bpath + "\\" + name[bf].alias + "\\";
			DIR* directory = opendir(folderPath.c_str());
		    if (!directory) {
		        printf("%s",Dialog[lang][40].c_str());
		        return ;
		    }
		    File files;
		    if(!choice)//寻找最新备份 
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
			                if(file.name[0] == 'Q') //如果是智能备份的存档，不处理
								continue;
			                file.modifiedTime = fileStat.st_mtime;
							if(file.modifiedTime > files.modifiedTime)
							{
								files.modifiedTime = file.modifiedTime;
								files.name = file.name;
							}
			            }
			        }
			    }
			    closedir(directory);
			}
			else
			{
				string folderName2 = Bpath + "\\" + name[bf].alias;
				printf("%s",Dialog[lang][41].c_str());
				ListFiles(folderName2);
				printf("%s",Dialog[lang][42].c_str());
				int bf2;
				scanf("%d",&bf2);
				files.name = temp[bf2];
				if(files.name[0] == 'Q') //如果是智能备份的存档，进行特殊的处理
					Qrestore(bf, bf2, false); 
			}
		    if(prebf)
		    	if(!smart) Backup(bf,false);
		    	else QuickBackup(bf,false);
		    if(files.name[0] != 'Q')
		    {
		    	command=yasuo+" x "+Bpath+"\\"+name[bf].alias+"\\"+files.name+" -o"+name[bf].real+" -y";
				system(command.c_str());
				sprint(Dialog[lang][43],30);
			}
		}
		else if(ch=='3')
		{
			SetConfig("config.ini",false,summ); 
		}
		else if(ch=='4')
		{
			printf("%s",Dialog[lang][44].c_str());
			int bf,tim;
			scanf("%d",&bf);
			printf("%s",Dialog[lang][45].c_str());
			scanf("%d",&tim);
			printf("%s%d",Dialog[lang][46].c_str(),tim);
			while(true)
			{
				if(!smart) Backup(bf,false);
				else QuickBackup(bf,false);
				Sleep(tim*60000);
			}
		}
		else if(ch=='5')
		{
			CreateConfig();
		}
		else printf("%s",Dialog[lang][47].c_str());
	}
	std::this_thread::sleep_for(std::chrono::seconds(1));
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
    CreateWindow("button", Dialog[lang][70].c_str(),
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        20, 10, 120, 35,
        hwnd, (HMENU)1, hInstance, NULL);

    CreateWindow("button", Dialog[lang][71].c_str(),
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        150, 10, 120, 35,
        hwnd, (HMENU)2, hInstance, NULL);

    CreateWindow("button", Dialog[lang][72].c_str(),
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        280, 10, 80, 35,
        hwnd, (HMENU)3, hInstance, NULL);

    ShowWindow(hwnd, nCmdShow);    
    std::thread MainThread(Main);
    MSG msg = {};
    
    //线程休眠，为了等待ontop读取完毕 
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if(ontop)
		SetWindowPos(hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE | SWP_NOSIZE);//Top the window
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    isRunning = false;
    MainThread.join();
    return 0;
}
