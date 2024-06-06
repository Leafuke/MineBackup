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
void sprint(string s,int time)//delayed output 
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

inline void neglect(int x)//read ignore x line 
{
	int num;
	char ch;
	while(num<x)
	{
		ch=getchar();
		if(ch=='\n') ++num;
	}
}

bool isDirectory(const std::string& path)//whether the folder exists or not 
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

//List subfolders 
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
//Get the last modified time of the folder 
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
//List files in the folder 
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
//pre-read (until;
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
		if(ch=='#') // if it's a comment line, then skip this line 
			while(ch!='\n')
				ch=getchar();	
	}
	return ;
}
// Get the registry value 
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
			printf("\n\nThis program calls 7-Zip for high compression backup, but you don't have 7-Zip installed on your computer yet, please go to the official website 7-zip.org to download or download the portable version of MineBackup first. \n\n");
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
string rname2[30],Bpath,command,yasuo,lv,format;//archive real name Backup folder path cmd command 7-Zip path Compression level 
bool prebf,ontop,choice,echos,smart;//backup before archive Toolkit top manually select Backup Smart Backup 
int limitnum;
HWND hwnd;
struct File {
    string name;
    time_t modifiedTime;
};
//Determine whether the file is occupied 
bool isFileLocked(const string& filePath)
{
    HANDLE hFile = CreateFile(filePath.c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_SHARE_READ, NULL);
    
    if (hFile == INVALID_HANDLE_VALUE)
    {
        DWORD error = GetLastError();
        // If the file is being opened exclusively by another process, the ERROR_SHARING_VIOLATION error is returned;
        if (error == ERROR_SHARING_VIOLATION)
            return true;
        // Other error cases need to be handled on a case-by-case basis
        else
            return false;
    }
    else
    {
        CloseHandle(hFile);
        // The file was successfully opened and closed, indicating that it is not currently occupied (at least at this instant)
        return false;
    }
}

//Archive folder path preprocessing 
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
//Checking aliases for plausibility
bool checkupName(string Name)
{
	int len=Name.size();
	for(int i=0;i<len;++i)
		if(Name[i]=='\\' || Name[i]=='/' || Name[i]==':' || Name[i]=='*' || Name[i]=='?' || Name[i]=='"' || Name[i]=='<' || Name[i]=='>' || Name[i]=='|')
			return false;
	return true;
}
//Detecting the number of backups
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
		string filePath = folderPath + fileName;; folderPath + fileName;
		struct stat fileStat;
		stat(filePath.c_str(), &fileStat);
	    if (S_ISREG(fileStat.st_mode)) {
    		 ++checknum;// if regular file, count total backups 
	    }
    }
    closedir(directory);
    struct dirent* entry2;
    while (checknum > limit)
    {
	    directory = opendir(folderPath.c_str());//putting it outside will cause read duplicates, it will only be deleted once, and won't be found after that;
		bool fl=0;
		while ((entry2 = readdir(directory))) {
		    string fileName = entry2->d_name;
		    string filePath = folderPath + fileName;; folderPath + fileName; folderPath + fileName;
		    struct stat fileStat;
		    if(!fl) files.modifiedTime=fileStat.st_mtime,fl=1; //reset files 
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
//Backup function 
void Backup(int bf,bool echo)
{
	string folderName = Bpath + "\\" + name[bf].alias; // Set folder name
	// Create a folder using mkdir ()
	mkdir(folderName.c_str());
	
	bool isFileLock = 0;
	if(isFileLocked(name[bf].real+"\\region\\\r.0.0.mca"))
	{
		isFileLock = true; 
		printf("The archive is detected as open and a temporary folder has been created under it. \n\nAll files in the archive folder are being copied to [1 temporary folder], then backup will begin \nPlease do not click at random during this process \n");
		command = "start \"\"\"" + name[bf].real + "\"\"";//this opens without reporting errors 
		system(command.c_str());
		Sleep(2000);
	    keybd_event(0x11, 0, 0, 0);//Ctrl
		keybd_event(0x41, 0, 0, 0);//A
		keybd_event(0x41, 0, KEYEVENTF_KEYUP, 0);
		keybd_event(0x43, 0, 0, 0);//C
		keybd_event(0x43, 0, KEYEVENTF_KEYUP, 0);
		keybd_event(0x11, 0, KEYEVENTF_KEYUP, 0);
		Sleep(500);
		string folderName2 = name[bf].real + "\\\1 Temporary Folder";
		mkdir(folderName2.c_str());
		command = "start \"\" \"" + folderName2 + "\"";
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
	    // Get a handle to the Copy Progress window
// 	Sleep(10);
	    HWND hForegroundWindow = GetForegroundWindow();
	    cout << "Copying..." << endl;
	    // Loop over the window to see if it still exists
	    int sumtime=0;
	    while (true) {
	        // Wait a while before checking to avoid high CPU usage
	        Sleep(2000); // wait 2 seconds
	        sumtime+=2;
	        // Check to see if the window is still active, or if it copied too fast and didn't catch the window at all;
	        if (!IsWindow(hForegroundWindow) || sumtime > 10)
	        {
	        cout << "File copy complete" << endl;
	        break; // Window closes, exiting the loop
			}
	    }
		Sleep(1000); 
		keybd_event(0x01, 0, 0, 0);//left button 
		keybd_event(0x01, 0, KEYEVENTF_KEYUP, 0);
		keybd_event(0x12, 0, 0, 0);//Alt 
		keybd_event(0x73, 0, 0, 0);//F4
		Sleep(100);
		keybd_event(0x01, 0, 0, 0);//left button 
		keybd_event(0x01, 0, KEYEVENTF_KEYUP, 0);
		keybd_event(0x73, 0, 0, 0);//F4
		keybd_event(0x73, 0, KEYEVENTF_KEYUP, 0);
		keybd_event(0x12, 0, KEYEVENTF_KEYUP, 0);
	}
	else // Record the block modification time during the backup, to facilitate the construction of fast compression in the future 
	{
		// Recorded only in QuickBackup now! 
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
	    Real+="\\\1Temporary Folder"; 
	if(echo) command=yasuo+" a -t "+format+" -mx="+lv+" "+tmp+" \""+Real+"\"\\*";
	else command=yasuo+" a -t "+format+" -mx="+lv+" "+tmp+" \""+Real+"\"\\* > nul 2>&1";
	//cout<< endl << command <<endl;//debug 
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
	freopen("CON", "w",stdout);
	return ;
}

//Smart Backup (Fast Backup) 
void QuickBackup(int bf, bool echo)
{
	string folderName = Bpath + "\\" + name[bf].alias; // Set folder name
	mkdir(folderName.c_str());
	
	// Not recorded yet, record the block modification time at the time of the backup;
	struct stat fileStat;
	string time1 = name[bf].real+"\\region\\r.0.0.mca";
	if(stat(time1.c_str(), &fileStat))//If no mca file exists, then it's a bedrock version of the archive, currently under-researched 
		return ;
	string accesss=name[bf].real+"\\Time.txt"; 
	struct dirent* entry;
	time1 = name[bf].real+"\\region";
	DIR* directory3 = opendir(time1.c_str());
	if(access(accesss.c_str(), F_OK) != 0)
	{
		time1 = name[bf].real+"\\Time.txt";
		ofstream newFile(time1);
		time1 = name[bf].real+"\\region";
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
    for(int j=0;j<com.size();++j) //initial processing of filenames 
	    if(j>=11 && j<=18)
    		 if(j==13 || j==16) tmp+="-";
    		 else tmp+=com[j];
    tmp="Q["+tmp+"]"+name[bf].alias; //quick backup filename multiple Qs 
    accesss = name[bf].real+"\\Time.txt"; 
	//freopen(accesses.c_str(), "r",stdin);
	ifstream usage;
	usage.open(accesss.c_str());
	string mca[500],number,backlist="";
	long long moditime[500],ii=0;
	while(moditime[ii] != -1) //read the time from Time.txt 
	{
		getline(usage,mca[++ii],' ');
		getline(usage,number,'\n');
		moditime[ii]=stoi(number); //convert number to integer 
	}
	freopen("CON", "r",stdin);
	time1 = name[bf].real+"\\region";
    while ((entry = readdir(directory3))) {// back up the new mca file
        string fileName = entry->d_name;
        string filePath = time1 + "\\" + fileName;
        if (stat(filePath.c_str(), &fileStat) != -1) {
            if (S_ISREG(fileStat.st_mode)) { // Only regular files are processed
				bool mcaok = false;
				for(int i=1;i<=ii;++i)
				{
					if(fileName == mca[i])
					{
						mcaok = true; 
						if(fileStat.st_mtime == moditime[i])
							mcaok = false;
					}
				}
				if(mcaok)
				{
					string Real = name[bf].real + "\\region\\" + fileName;
					backlist = backlist + " \"" + Real + "\"";
				}
            }
        }
    }
    if(backlist.size()<=5)//no changes made 
		return ; 
    if(echo) command=yasuo+" a -t "+format+" -mx="+lv+""+tmp+backlist;
	else command=yasuo+" a -t "+format+" -mx="+lv+" "+tmp+backlist+" > nul 2>&1";
	system(command.c_str());
	if(echo) command="move "+tmp+".7z "+folderName;
	else command="move "+tmp+".7z "+folderName+" > nul 2>&1";
	system(command.c_str());
	checkup(folderName+"\\",limitnum);
	freopen("CON", "w",stdout);
	
	directory3 = opendir(time1.c_str());
	// Record the backup time again 
	time1 = name[bf].real+"\\Time.txt";
	ofstream newFile(time1);
	time1 = name[bf].real+"\\region";
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

// Cleverly return environment variables 
string GetSpPath(string sp)
{
	freopen("temp", "w",stdout);
	system(sp.c_str());
	ifstream tmp("temp");
	freopen("CON", "w",stdout);
	string ans;
	getline(tmp, ans, '\n');
// system("del temp"); this will cause freopen to error out;
	return ans;
}

//Initial settings/Update settings 
void SetConfig(string filename, bool ifreset, int summ)
{
	// Now consolidate the creation of the configuration file into a single function SetConfig() 
	freopen("CON", "r",stdin);
	cin.clear();
	printf("\n is building configuration file ...... \n");
	ofstream newFile(filename);
	if(ifreset)
	{
		printf("Trying to find known game archive path... \n");
		string searchPath = "", searchTemp = GetSpPath("echo %Appdata%") + "\\.minecraft\\saves";
		if(isDirectory(searchTemp)) // For environment variables like %Appdata%, special handling is required 
		{
			printf("Found official launcher JAVA version archive, is it added to the path? (Yes:1 No:0)\n");
			char ifadd = getch();
			if(ifadd == '1')
				searchPath += "$" + searchTemp;
		}
		else
			printf("JAVA version of the archive was not found, if it exists, please enter it manually afterwards\n");
		searchTemp = GetSpPath("echo %LOCALAPPDATA%") + "\\Packages\\Microsoft.MinecraftUWP_8wekyb3d8bbwe\\LocalState\\games\\com.mojang\\ minecraftWorlds";
		if(isDirectory(searchTemp)) 
		{
			printf("Found the official launcher bedrock version archive, is it added to the path? (Yes:1 No:0)\n");
			char ifadd = getch();
			if(ifadd == '1')
				searchPath += "$" + searchTemp;
		}
		else
			printf("No bedrock version of the archive found. \n");
		printf("Please enter the path to the folder where the archive folder is stored (written on one line, multiple folder paths are separated by $): ");
		getline(cin,Gpath);
		Gpath += searchPath; 
		printf("Please enter the path to the archive backup storage:");
		getline(cin,Bpath);
		summ=PreSolve(Gpath);
		system("del temp");
	}
	else
	{
		Gpath="";
		for(int i=0;i<summ;++i)
	        Gpath+=Gpath2[i],Gpath+="$";
        Gpath+=Gpath2[summ];
	}	
	
    if (newFile.is_open()) {
	    newFile << "Used profile serial number:0" << endl;
	    newFile << "Archive folder path:" << Gpath << endl;//new
        newFile << "Archived backup storage path:" << Bpath << endl;
		string keyPath = "Software\\\7-Zip"; 
		string valueName = "Path";
		string softw=GetRegistryValue(keyPath, valueName),softww="";
		for(int i=0;i<softw.size();++i)
			if(softw[i]==' ') softww+='"',softww+=' ',softww+='"';
			else softww+=softw[i];
        newFile << "Path to compression software:" << softww+"7z.exe" << endl;
        newFile << "Restore before backing up:0" << endl;
        newFile << "Toolbox topping:0" << endl;
        newFile << "Manually select restore (default is latest):0" << endl;
        newFile << "Procedure display:1" << endl;
        newFile << "Compression Format:7z" << endl;
        newFile << "Compression level:5" << endl;
        newFile << "Number of backups retained (0 means unlimited):0" << endl;
        newFile << "SmartBackup:0" << endl;
	}
	printf("\n has the following archive folders:\n\n"); 
	for(int i=0;i<=summ;++i)
	{
		bool ifalias=true; // Whether to set aliases manually 
		cout << endl;
		std::vector<std::string> subdirectories;
		listSubdirectories(Gpath2[i], subdirectories);
	    for (const auto& folderName : subdirectories)
	    {
			std::string NGpath=Gpath2[i]+"\\"+folderName;
	        std::string modificationDate = getModificationDate(NGpath);
	        std::cout << "Archive name: " << folderName << endl;
	        std::cout << "Recent playtime: " << modificationDate << endl;
	        std::cout << "-----------" << endl;
	    }
	    Sleep(1000);
	    sprint("Do you wish to set aliases for all archives? (0/1)\n\n",30);
	    cin>>ifalias; 
	    if(ifalias) sprint("Next, you need to give these folders aliases that are easy for you to understand on your own\n\n",30);
		else sprint("Then it will automatically take the archive folder name as alias, if you need to change the alias, please change it manually in \"Settings\". \nIf you need to change the alias, please change it manually in \"Settings\".",10);
		for (const auto& folderName : subdirectories)
	    {
	        string alias;
	        B:
	        if(ifalias)
			{
				cout << "Please enter an alias for the following archive (can be a description) " << folderName << endl;
	        cin >> alias;
			}
			else alias = folderName;
	        if(!checkupName(alias))
			{
				printf("Folder name cannot contain the symbol \\ / : * ?  \" < > |, please rename it");;
				goto B;
			}
			newFile << folderName << endl << alias << endl;
	    }
	    newFile << "$" << endl;
	}
    newFile << "*" << endl;
    newFile.close();
    return ;
}

//Creating a new backup file 
void CreateConfig()
{
	printf("\nYou need to create (1) General Configuration or (2) Fully Automatic Configuration\n\n");;
	char ch=getch();
	string folderName,filename = "config1.ini";
	string i="1";
    ifstream file(filename);
    while(true)
    {
	    i[0]+=1;
	    filename="config "+i+".ini";
	    ifstream file(filename);
	    if(!file.is_open()) break;
	}
	if(ch=='1')
	{
		printf("\n is creating a configuration file named %s\n",&filename[0]);;     
	    ofstream newFile(filename);
	    printf("Please enter the storage path of the archive folder (multiple folder paths are separated by $): ");
		getline(cin,Gpath);
		printf("Please enter the path to the backup storage folder:");
		getline(cin,Bpath);
		for(int i=0;i<=10;++i)
			Gpath2[i]="";
		int summ=PreSolve(Gpath);
        if (newFile.is_open()) {
	        newFile << "Auto:0" << endl;
	        newFile << "All archive paths:" << Gpath2[0];
	        if(summ>1) newFile << '$'; 
	        for(int i=1;i<summ;++i)
        		 newFile << Gpath2[i] << '$';
	        if(summ!=0) newFile << Gpath2[summ] << endl;
	        else newFile << endl;
            newFile << "Backup Storage Path:" << Bpath << endl;
			string keyPath = "Software\\7-Zip"; 
			string valueName = "Path";
			string softw=GetRegistryValue(keyPath, valueName),softww="";
			for(int i=0;i<softw.size();++i)
				if(softw[i]==' ') softww+='"',softww+=' ',softww+='"';
				else softww+=softw[i];
            newFile << "Path to compression software:" << softww+"7z.exe" << endl;
            newFile << "Backup before restoring:0" << endl;
            newFile << "Toolbox topping:0" << endl;
            newFile << "Manually selecting backup:0" << endl;
            newFile << "Procedure display:1" << endl;
            newFile << "Compression Format:7z" << endl;
	        newFile << "Compression level:5" << endl;
	        newFile << "Number of backups retained (0 means unlimited):0" << endl;
	        newFile << "SmartBackup:0" << endl;
	    }
	    printf("\n has the following archive:\n\n");
	    for(int i=0;i<=summ;++i)
	    {
    		 cout << endl;
    		 std::vector<std::string> subdirectories;
			listSubdirectories(Gpath2[i], subdirectories);
		    for (const auto& folderName : subdirectories)
		    {
				std::string NGpath=Gpath2[i]+"\\"+folderName;
		        std::string modificationDate = getModificationDate(NGpath);
		        std::cout << "Archive name: " << folderName << endl;
		        std::cout << "Recent play time " << modificationDate << endl;
		        std::cout << "-----------" << endl;
		    }
		    Sleep(2000);
		    sprint("Next, you need to give these folders aliases that are easy for you to understand on your own\n\n",50);
			for (const auto& folderName : subdirectories)
		    {
		        string alias;
		        cout << "Please set an alias for the following archive (can be a description) " << folderName << endl;
		        cin >> alias;
				newFile << folderName << endl << alias << endl;
		    }
		    newFile << "$" << endl;
		}
	    newFile << "*" << endl;
	    newFile.close();
	    sprint("Configuration file creation complete!!!! \n",10);
        return ;
	}
	else if(ch=='2')
	{
		ofstream newFile(filename);
		newFile << "AUTO:1" << endl;
		int configs;
		printf("Configuration file number to be called (from which to get the archive name and alias):\n");;
		cin>>configs;
		newFile << "Use Config:" << configs << endl;
		printf("Need to back up the first archive:");
		cin>>configs;
		newFile << "BF:" << configs << endl;
		printf("Do you need (1) a timed backup or (2) an intermittent backup\n");
		ch=getch();
		if(ch=='1'){
			printf("Enter the time at which you want to back up: 1Please enter the month and enter (enter 0 for every month):");
			int mon,day,hour,min;
			scanf("%d",&mon);
			printf("2Please enter the date, and then enter (enter 0 for every day):");
			scanf("%d",&day);
			printf("3Please enter the hour and then enter (enter 0 for every hour):");
			scanf("%d",&hour);
			printf("4Please enter the minutes and then enter:");
			scanf("%d",&min);
			newFile << "Mode:1\nTime:" << mon << " " << day << " " << hour << " " << min << endl;
		} 
		else if(ch=='2'){
			printf("Enter how many minutes you want to back up between backups:");
			int detime;
			scanf("%d",&detime);
			newFile << "Mode:2\nTime:" << detime << endl;
		}
		else{
			printf("\nError\n");
			return ;
		}
		printf("Whether or not to enable the no-disturb mode (0/1):");
		cin>>configs;
		newFile << "Inter:" << configs << "\n*";
		sprint("Configuration file creation complete!!!! \n",10);
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
	    // Now consolidate the creation of the configuration file into a single function SetConfig() 
		SetConfig(filename, true, 0);
        return ;
    }
    
    freopen("config.ini", "r",stdin);
    Qread();
    string temps,tempss;
    int configs;
    char configss[100];
	scanf("%d",&configs);
	if(configs!=0)
	{
		temps=to_string(configs); //only c++11
		temps="config "+temps+".ini";
		strcpy(configss,temps.c_str());
		freopen(configss, "r",stdin);
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
				scanf("%d %d %d %d %d",&month,&day,&hour,&min);//Here if read in error, followed by correct .....;
				//2023.12.31 solve it, just one more string after the config file 
			}
			else if(mode==2)
			{
				Qread();
				cin>>bftime;
			}
			Qread();
			cin>>ifinter;
			if(usecf==0) // use the archive path from the general configuration 
			{
				freopen("config.ini", "r",stdin);
//				 getline(cin,tmp1);// Problem Here
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
				memset(inputs,'\0',sizeof(inputs));;
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
				memset(inputs,'\0',sizeof(inputs));;
				inputs[0] = getchar(),inputs[1] = getchar();
				format = inputs;
				Qread();
				memset(inputs,'\0',sizeof(inputs));;
				inputs[0] = getchar();
				lv = inputs;
				Qread();
				cin >> smart;
				Qread();
				cin >> limitnum;
			    int i = 0,ttt = 0;//number of archives Serial number of the archive folder where the archive is located 
			    inputs[0] = getchar();// addition
			    while(true)
			    {
					memset(inputs,'\0',sizeof(inputs));;
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
			    memset(inputs,'\0',sizeof(inputs));;
					for(int i=0;;++i)
					{
						inputs[i]=getchar();
						if(inputs[i]=='\n'){
							inputs[i]='\0';break;
						}
					}
					name[i].alias=inputs;
				}
				//Detect if the archive folder has been updated 
				int ix=0;
				bool ifnew=0; 
				for(int i=0;i<=summ;++i)
				{
					std::vector<std::string> subdirectories;
					listSubdirectories(Gpath2[i], subdirectories);
				    for (const auto& folderName : subdirectories)
				    {
				    if((Gpath2[i]+"/"+folderName)==name[++ix].real) //doesn't update if it matches real Note that the detection here is flawed 
				    		 continue;
						--ix;// -1 on comparison because of one extra, but fails to deal with the reduced case 
				    // Inconsistent, then list
				    ifnew=true;
						printf("\n New archive detected as follows:\n");
						string NGpath=Gpath2[i]+"/"+folderName;
				        string modificationDate = getModificationDate(NGpath);
				        cout << "Archive Name: " << folderName << endl;
				        cout << "Recent playtime: " << modificationDate << endl;
				        cout << "-----------" << endl;
				    }
				}
				if(ifnew) // if there is an update, ask if the profile is updated
				{
					printf("Please update the configuration file? (0/1)\n");
					char ch;
					ch=getch();
					printf("\nPlease manually update the \"real name\" and \"alias\"\n 	of the new archive by adding them after the corresponding location");
					if(ch=='1')
						system("start config.ini");
				}
			}
			else
			{
				string temps=to_string(usecf);
			    char configss[10];
				temps="config "+temps+".ini";
				for(int i=0;i<temps.size();++i)
					configss[i] = temps[i];
				freopen(configss, "r",stdin);
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
				//Detect if the archive folder has been updated 
				int ix=0;
				bool ifnew=0; 
				for(int i=0;i<=summ;++i)
				{
					std::vector<std::string> subdirectories;
					listSubdirectories(Gpath2[i], subdirectories);
				    for (const auto& folderName : subdirectories)
				    {
				    if((Gpath2[i]+"/"+folderName)==name[++ix].real) //doesn't update if it matches real Note that the detection here is flawed 
				    		 continue;
						--ix;// -1 on comparison because of one extra, but fails to deal with the reduced case 
				    // Inconsistent, then list
				    ifnew=true;
						printf("\n New archive detected as follows:\n");
						string NGpath=Gpath2[i]+"\\"+folderName;
				        string modificationDate = getModificationDate(NGpath);
				        cout << "Archive Name: " << folderName << endl;
				        cout << "Last playtime: " << modificationDate << endl;
				        cout << "-----------" << endl;
				    }
				}
				if(ifnew) // if there is an update, ask if the profile is updated
				{
					printf("Please update the configuration file? (0/1)\n");
					char ch;
					ch=getch();
					printf("\nPlease manually update the \"real name\" and \"alias\"\n of the new archive by adding them after the corresponding location");
					string command="start "+temps;
					if(ch=='1')
						system(command.c_str());
				}
			}
			if(mode==1)
			{
				while(true)
				{
					// Get the current time
				    std::time_t now = std::time(nullptr);
				    std::tm* local_time = std::localtime(&now);
				
				    std::tm target_time = {0};
				    target_time.tm_year = local_time->tm_year; // Years are calculated from 1900 onwards
				    if(month==0) target_time.tm_mon = local_time->tm_mon;
				    else target_time.tm_mon = month - 1; // month starts at 0
				    if(day==0) target_time.tm_mday = local_time->tm_mday;
				    else target_time.tm_mday = day;
				    if(hour==0) target_time.tm_hour = local_time->tm_hour;
				    else target_time.tm_hour = hour;
				    if(min==0) target_time.tm_min = local_time->tm_min;
				    else target_time.tm_min = min;
				    // If the current time has already exceeded the target time, then there is no need to wait
				    A:
				    if (std::mktime(local_time) > std::mktime(&target_time)) {
				        std::cout << "The time has now passed the target time" << std::endl;
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
				        // Calculate the amount of time to wait (in seconds)
				        std::time_t wait_time = std::difftime(std::mktime(&target_time), std::mktime(local_time));
				        // Wait for the specified time
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
				    // Let the thread sleep for a specified amount of time
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
    int i=0,ttt=0;//number of archives serial number of the archive folder where the archive is located 
    printf("The following archive is available:\n\n");
    char ch=getchar();//DEBUG because getline ..//bug why?now ok?;
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
	    printf("%d.",i);
	    cout<<name[i].alias<<endl;
	}
	freopen("CON", "r",stdin);
	//Detect if the archive folder has been updated 
	int ix=0;
	bool ifnew=0; 
	for(int i=0;i<=summ;++i)
	{
		std::vector<std::string> subdirectories;
		listSubdirectories(Gpath2[i], subdirectories);
	    for (const auto& folderName : subdirectories)
	    {
	    if((Gpath2[i]+"\\"+folderName)==name[++ix].real) //doesn't update if it matches real Note that the detection here is flawed 
	    		 continue;
			--ix;// -1 on comparison because of one extra, but fails to deal with the reduced case 
	    // Inconsistent, then list
	    ifnew=true;
			printf("\n New archive detected as follows:\n");
			string NGpath=Gpath2[i]+"\\"+folderName;
	        string modificationDate = getModificationDate(NGpath);
	        cout << "Archive Name: " << folderName << endl;
	        cout << "Recent playtime: " << modificationDate << endl;
	        cout << "-----------" << endl;
	    }
	}
	if(ifnew) // if there is an update, ask if the profile is updated
	{
		printf("Please update the configuration file? (0/1)\n");
		char ch;
		ch=getch();
		printf("\nPlease manually update the \"real name\" and \"alias\"\n of the new archive by adding them after the corresponding location");; 
		if(ch=='1')
			system("start config.ini");
	}
	while(true)
	{
		printf("Do you want to (1) backup the archive (2) go back to the archive (3) update the archive (4) make an automatic backup or (5) create a configuration file? (Press 1/2/3/4/5)\n");
		char ch;
		ch=getch();
		if(ch=='1')
		{
			printf("Enter the serial number before the archive to complete the backup:");
			int bf;
			scanf("%d",&bf);
			if(!smart) Backup(bf,echos);
			else QuickBackup(bf,echos);
			sprint("\n\n Backup complete! ! ! ! \n\n",40);
		}
		else if(ch=='2')
		{
			int i=0;
		    if(!choice) printf("Enter the serial number of the pre-archive to complete the restore (mode: select latest backup) : ");
		    else printf("Enter the serial number before the archive to complete the restore (Mode: Manual Selection) :");
			int bf;
			scanf("%d",&bf);
			string folderPath=Bpath+"\\"+name[bf].alias+"\\";
			DIR* directory = opendir(folderPath.c_str());
		    if (!directory) {
		        printf("Backup does not exist and could not be restored! You probably haven't made a backup \n");
		        return ;
		    }
		    File files;
		    if(!choice)//find latest backups 
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
				string folderName2 = Bpath + "\\" + name[bf].alias;
				printf("The following is a backup archive \n\n");
				ListFiles(folderName2);
				printf("Enter the serial number before the backup to complete the restoration:");
				int bf2;
				scanf("%d",&bf2);
				files.name=temp[bf2];
			}
		    if(prebf)
		    if(!smart) Backup(bf,false);
		    else QuickBackup(bf,false);
			command=yasuo+" x "+Bpath+"\\"+name[bf].alias+"\\"+files.name+" -o "+name[bf].real+" -y";
			system(command.c_str());
			sprint("\n\n Restore Successful! ! ! ! !\n\n",40);
		}
		else if(ch=='3')
		{
			SetConfig("config.ini",false,summ);
		}
		else if(ch=='4')
		{
			printf("Please enter the serial number of the archive you wish to back up:");
			int bf,tim;
			scanf("%d",&bf);
			printf("Backups every few minutes: ");
			scanf("%d",&tim);
			printf("Entered automatic backup mode, backing up every %d minutes",tim);
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
		else printf("Please press the 1/2/3/4/5 key on the keyboard \n\n");
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
    CreateWindow("button", "Archive Folder",
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        20, 10, 120, 35,
        hwnd, (HMENU)1, hInstance, NULL);

    CreateWindow("button", "Backup Folder",
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        150, 10, 120, 35,
        hwnd, (HMENU)2, hInstance, NULL);

    CreateWindow("button", "Settings",
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        280, 10, 80, 35,
        hwnd, (HMENU)3, hInstance, NULL);

    ShowWindow(hwnd, nCmdShow);    
    std::thread MainThread(Main);
    MSG msg = {};
    
    //The thread sleeps, in order to wait for the ontop to finish reading;
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
